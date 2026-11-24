#include "dev_test_helper.h"

#ifdef APP_DEV_TEST_HELPER

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cmd_context.h"
#include "cmd_net_job.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "mqtt.h"
#include "operation.h"
#include "wifi.h"

#if defined(APP_CMD_NET_JOB_TEST)
#error "APP_DEV_TEST_HELPER must not be combined with APP_CMD_NET_JOB_TEST"
#endif

#if defined(APP_DEV_TEST_SCENARIO_T13)
#define DEVTEST_SCENARIO_NAME "t13"
#elif defined(APP_DEV_TEST_SCENARIO_T19)
#define DEVTEST_SCENARIO_NAME "t19"
#elif defined(APP_DEV_TEST_SCENARIO_WAIT200)
#define DEVTEST_SCENARIO_NAME "wait200"
#else
#error "APP_DEV_TEST_HELPER requires one APP_DEV_TEST_SCENARIO_* definition"
#endif

static const char *TAG = "devtest";

#define DEVTEST_TASK_STACK_BYTES 4096
#define DEVTEST_TASK_STACK_DEPTH (DEVTEST_TASK_STACK_BYTES / sizeof(StackType_t))
#define DEVTEST_TASK_PRIO 4
#define DEVTEST_NET_TIMEOUT_MS 20000
#define DEVTEST_ADMISSION_TIMEOUT_MS 15000
#define DEVTEST_BARRIER_TIMEOUT_MS 2000
#define DEVTEST_FINISH_TIMEOUT_MS 5000
#define DEVTEST_TERMINAL_TIMEOUT_MS 5000
#define DEVTEST_WORKER_SETTLE_MS 100
#define DEVTEST_RUN_TIMEOUT_MS 15000

#define EV_RUN_STARTED   (1U << 0)
#define EV_RUN_RELEASE   (1U << 1)
#define EV_RUN_EXIT      (1U << 2)
#define EV_BARRIER_HIT   (1U << 3)
#define EV_BARRIER_OPEN  (1U << 4)
#define EV_FINISH_START  (1U << 5)
#define EV_FINISH_DONE   (1U << 6)
#define EV_FIRST_CLEANUP_DONE (1U << 7)

typedef enum {
    DEV_FINISH_NOT_STARTED = 0,
    DEV_FINISH_CALL_STARTED,
    DEV_FINISH_HOOK_REACHED,
    DEV_FINISH_RETURNED,
} dev_finish_state_t;

static EventGroupHandle_t s_events;
static StaticEventGroup_t s_events_buf;
static StaticTask_t s_control_tcb;
static StackType_t s_control_stack[DEVTEST_TASK_STACK_DEPTH];
#if defined(APP_DEV_TEST_SCENARIO_T13) || defined(APP_DEV_TEST_SCENARIO_WAIT200)
static StaticTask_t s_claim_tcb;
static StackType_t s_claim_stack[DEVTEST_TASK_STACK_DEPTH];
#endif
static TaskHandle_t s_control_task;
#if defined(APP_DEV_TEST_SCENARIO_T13) || defined(APP_DEV_TEST_SCENARIO_WAIT200)
static TaskHandle_t s_claim_task;
#endif

static app_operation_t *s_op;
static char s_op_id[APP_OPERATION_ID_LEN];
static uint32_t s_generation;
static char s_run_id[24];
static bool s_started;
static const char *s_admission_reason = "none";
static bool s_admission_skip;
#if defined(APP_DEV_TEST_SCENARIO_T13) || defined(APP_DEV_TEST_SCENARIO_WAIT200)
static bool s_hook_used;
#endif

static volatile dev_finish_state_t s_finish_state = DEV_FINISH_NOT_STARTED;
static volatile app_operation_finish_result_t s_finish_result =
    APP_OPERATION_FINISH_STALE;
static volatile bool s_finish_result_valid;
static volatile bool s_first_cleanup_result;
static volatile bool s_first_cleanup_started;
static volatile bool s_finish_task_created;
static volatile bool s_first_cleanup_task_created;
static volatile bool s_run_started;

static bool devtest_net_ready(void)
{
    bool mqtt_connected;

    app_mqtt_lock();
    mqtt_connected = s_mqtt.connected;
    app_mqtt_unlock();
    return s_wifi.sta_has_ip && mqtt_connected;
}

static void devtest_log_net_state(const char *tag)
{
    bool mqtt_connected;

    app_mqtt_lock();
    mqtt_connected = s_mqtt.connected;
    app_mqtt_unlock();
    ESP_LOGI(TAG, "DEVTEST_NET %s sta_has_ip=%d mqtt_connected=%d",
             tag, (int)s_wifi.sta_has_ip, (int)mqtt_connected);
}

static bool devtest_wait_net_ready(void)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)DEVTEST_NET_TIMEOUT_MS * 1000;
    int64_t next_log = 0;

    while (esp_timer_get_time() < deadline) {
        if (devtest_net_ready()) {
            devtest_log_net_state("ready");
            return true;
        }
        if (esp_timer_get_time() >= next_log) {
            devtest_log_net_state("waiting");
            next_log = esp_timer_get_time() + 2000000;
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    devtest_log_net_state("timeout");
    return false;
}

static bool devtest_wait_status_terminal(uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;

    while (esp_timer_get_time() < deadline) {
        app_operation_status_t st;
        if (app_operation_get_status(s_op, &st)) {
            if (st.state == APP_OP_SUCCESS || st.state == APP_OP_FAILED ||
                st.state == APP_OP_CANCELLED || st.state == APP_OP_EXPIRED) {
                return true;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return false;
}

static bool devtest_wait_claim_settled(app_operation_cleanup_state_t *out,
                                       uint32_t timeout_ms)
{
    int64_t deadline = esp_timer_get_time() + (int64_t)timeout_ms * 1000;
    app_operation_cleanup_state_t c = APP_OPERATION_CLEANUP_NONE;

    while (esp_timer_get_time() < deadline) {
        if (!app_operation_generation_matches(s_op, s_generation)) {
            ESP_LOGE(TAG, "DEVTEST_IDENTITY_ERROR cleanup_state_stale=1 "
                          "cleanup_wait_generation op=%s gen=%u",
                     s_op_id, (unsigned)s_generation);
            if (out != NULL) {
                *out = APP_OPERATION_CLEANUP_NONE;
            }
            return false;
        }
        c = app_operation_cleanup_result(s_op, s_generation);
        if (c == APP_OPERATION_CLEANUP_OK || c == APP_OPERATION_CLEANUP_FAIL) {
            if (out != NULL) {
                *out = c;
            }
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    if (out != NULL) {
        *out = c;
    }
    return false;
}

static void devtest_release_barrier(void)
{
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, EV_BARRIER_OPEN);
    }
}

static void devtest_release_run(void)
{
    if (s_events != NULL) {
        xEventGroupSetBits(s_events, EV_RUN_RELEASE);
    }
}

static bool devtest_wait_run_exit(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_RUN_EXIT, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
    return (bits & EV_RUN_EXIT) != 0;
}

static void devtest_hook(const char *point, app_operation_t *op,
                         uint32_t expected_generation)
{
#if defined(APP_DEV_TEST_SCENARIO_T13) || defined(APP_DEV_TEST_SCENARIO_WAIT200)
    if (point == NULL || strcmp(point, "cleanup_after_claim") != 0) {
        return;
    }
    if (op != s_op || expected_generation != s_generation) {
        return;
    }
    if (s_hook_used) {
        ESP_LOGW(TAG, "DEVTEST_HOOK duplicate point=%s", point);
        return;
    }
    s_hook_used = true;
    if (s_finish_state == DEV_FINISH_CALL_STARTED) {
        s_finish_state = DEV_FINISH_HOOK_REACHED;
    }
    ESP_LOGI(TAG, "DEVTEST_HOOK point=%s op=%s gen=%u", point, s_op_id,
             (unsigned)s_generation);
    xEventGroupSetBits(s_events, EV_BARRIER_HIT);
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_BARRIER_OPEN, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(DEVTEST_BARRIER_TIMEOUT_MS));
    if ((bits & EV_BARRIER_OPEN) == 0) {
        ESP_LOGE(TAG, "DEVTEST_HOOK barrier timeout op=%s", s_op_id);
    } else {
        ESP_LOGI(TAG, "DEVTEST_HOOK released op=%s", s_op_id);
    }
#else
    (void)point;
    (void)op;
    (void)expected_generation;
#endif
}

static esp_err_t devtest_run_fn(app_operation_t *op, void *ctx)
{
    (void)op;
    (void)ctx;
    s_run_started = true;
    xEventGroupSetBits(s_events, EV_RUN_STARTED);
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_RUN_RELEASE, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(DEVTEST_RUN_TIMEOUT_MS));
    if ((bits & EV_RUN_RELEASE) == 0) {
        ESP_LOGE(TAG, "DEVTEST_RUN release timeout op=%s", s_op_id);
        xEventGroupSetBits(s_events, EV_RUN_EXIT);
        return ESP_ERR_TIMEOUT;
    }
    ESP_LOGI(TAG, "DEVTEST_RUN exit op=%s", s_op_id);
    xEventGroupSetBits(s_events, EV_RUN_EXIT);
    return ESP_OK;
}

static bool devtest_identity_failure_cleanup(esp_err_t cancel_err)
{
    app_operation_status_t st = {0};
    app_cmd_net_job_snap_t job;
    app_cmd_net_trial_snap_t trial;
    bool status_ok;
    bool id_ok;
    bool terminal_ok;
    bool cleanup_ok;
    bool resources_ok;

    app_cmd_net_job_lock();
    app_cmd_net_job_snapshot_locked(&job);
    app_cmd_net_trial_snapshot_locked(&trial);
    app_cmd_net_job_unlock();

    status_ok = app_operation_get_status(s_op, &st);
    id_ok = status_ok && strcmp(st.id, s_op_id) == 0;
    terminal_ok = status_ok &&
                  (st.state == APP_OP_CANCELLED || st.state == APP_OP_FAILED ||
                   st.state == APP_OP_EXPIRED || st.state == APP_OP_SUCCESS);
    cleanup_ok = status_ok && st.state == APP_OP_CANCELLED &&
                 st.cleanup_state == APP_OPERATION_CLEANUP_OK;
    resources_ok = !job.active && !trial.valid;
    bool converged = cancel_err == ESP_OK && id_ok && terminal_ok &&
                     cleanup_ok && resources_ok;

    ESP_LOGE(TAG, "DEVTEST_CLEANUP identity_read_failed cancel=%s status_ok=%d "
                  "id_ok=%d state=%d cleanup=%d job_active=%d trial_valid=%d "
                  "outcome=%s op=%s",
             esp_err_to_name(cancel_err), (int)status_ok, (int)id_ok,
             (int)st.state, (int)st.cleanup_state, (int)job.active,
             (int)trial.valid, converged ? "PASS" : "FAIL", s_op_id);
    if (!converged) {
        ESP_LOGE(TAG, "DEVTEST_IDENTITY_ERROR cleanup_state_stale=1 "
                      "identity_failure_cleanup op=%s", s_op_id);
    }
    return converged;
}

static bool devtest_begin_operation(void)
{
    app_cmd_context_t ctx = app_cmd_ctx_entry(APP_CMD_SOURCE_SERIAL);
    char idem_key[40];
    int64_t deadline = esp_timer_get_time() + (int64_t)DEVTEST_ADMISSION_TIMEOUT_MS * 1000;
    unsigned attempts = 0;
    unsigned no_txn_attempts = 0;

    s_admission_reason = "none";
    s_admission_skip = false;
    if (!app_cmd_ctx_valid(&ctx)) {
        s_admission_reason = "invalid_ctx";
        ESP_LOGE(TAG, "DEVTEST_ADMISSION result=invalid_ctx");
        return false;
    }
    snprintf(idem_key, sizeof(idem_key), "devtest-%s", s_run_id);

    while (esp_timer_get_time() < deadline) {
        app_cmd_net_job_begin_result_t r;
        app_operation_t *op = NULL;
        app_operation_status_t st = {0};

        attempts++;
        app_cmd_net_job_lock();
        r = app_cmd_net_job_begin_locked(APP_CMD_SOURCE_SERIAL,
                                         APP_OPERATION_OWNER_INTERNAL,
                                         &ctx, "devtest-ssid", "devtest-pass",
                                         idem_key, 30000, 30000,
                                         devtest_run_fn, &op);
        app_cmd_net_job_unlock();

        if (r == APP_CMD_NET_JOB_BEGIN_OK) {
            if (op == NULL) {
                s_admission_reason = "op_null";
                ESP_LOGE(TAG, "DEVTEST_ADMISSION result=op_null attempts=%u", attempts);
                return false;
            }
            /* BEGIN_OK 后立即保存身份，状态读取失败也必须走受控收敛。 */
            s_op = op;
            snprintf(s_op_id, sizeof(s_op_id), "%s", app_operation_id(op));
            for (unsigned i = 0; i < 3; i++) {
                if (app_operation_get_status(op, &st)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(10));
            }
            if (st.generation == 0 || st.cleanup_state != APP_OPERATION_CLEANUP_NONE) {
                s_admission_reason = "identity_error";
                ESP_LOGE(TAG, "DEVTEST_IDENTITY_ERROR admission op=%s", s_op_id);
                esp_err_t cancel_err = app_operation_cancel(op);
                (void)devtest_identity_failure_cleanup(cancel_err);
                return false;
            }
            s_generation = st.generation;
            s_admission_reason = "ok";
            ESP_LOGI(TAG, "DEVTEST_ADMISSION result=ok attempts=%u op=%s gen=%u",
                     attempts, s_op_id, (unsigned)s_generation);
            return true;
        }
        if (r == APP_CMD_NET_JOB_BEGIN_BUSY) {
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        if (r == APP_CMD_NET_JOB_BEGIN_NO_TXN) {
            no_txn_attempts++;
            if (no_txn_attempts < 3) {
                vTaskDelay(pdMS_TO_TICKS(100));
                continue;
            }
            s_admission_reason = "cfg_txn_unavailable";
            s_admission_skip = true;
            ESP_LOGE(TAG, "DEVTEST_ADMISSION result=no_txn attempts=%u", attempts);
            return false;
        }
        if (r == APP_CMD_NET_JOB_BEGIN_NOT_READY) {
            s_admission_reason = "not_ready";
            ESP_LOGE(TAG, "DEVTEST_ADMISSION result=not_ready attempts=%u", attempts);
            return false;
        }
        s_admission_reason = "no_slot";
        ESP_LOGE(TAG, "DEVTEST_ADMISSION result=no_slot attempts=%u", attempts);
        return false;
    }

    s_admission_reason = "busy_timeout";
    ESP_LOGE(TAG, "DEVTEST_ADMISSION timeout attempts=%u", attempts);
    return false;
}

static bool devtest_wait_connecting(void)
{
    EventBits_t bits = xEventGroupWaitBits(s_events, EV_RUN_STARTED, pdFALSE, pdTRUE,
                                           pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
    app_operation_status_t st;

    if ((bits & EV_RUN_STARTED) == 0) {
        ESP_LOGE(TAG, "DEVTEST_RUN_START timeout op=%s", s_op_id);
        return false;
    }
    if (!app_operation_get_status(s_op, &st)) {
        ESP_LOGE(TAG, "DEVTEST_RUN_STATUS unavailable op=%s", s_op_id);
        return false;
    }
    if (st.state != APP_OP_CONNECTING || st.generation != s_generation) {
        ESP_LOGE(TAG, "DEVTEST_RUN_STATE state=%d gen=%u expected_gen=%u",
                 (int)st.state, (unsigned)st.generation, (unsigned)s_generation);
        return false;
    }
    ESP_LOGI(TAG, "DEVTEST_RUN_CONNECTING op=%s gen=%u", s_op_id,
             (unsigned)s_generation);
    return true;
}

#if defined(APP_DEV_TEST_SCENARIO_T13)
static void devtest_finish_task(void *arg)
{
    (void)arg;
    s_finish_state = DEV_FINISH_CALL_STARTED;
    xEventGroupSetBits(s_events, EV_FINISH_START);
    s_finish_result = app_operation_request_finish(
        s_op, s_generation, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    s_finish_result_valid = true;
    s_finish_state = DEV_FINISH_RETURNED;
    xEventGroupSetBits(s_events, EV_FINISH_DONE);
    vTaskDelete(NULL);
}

#endif

#if defined(APP_DEV_TEST_SCENARIO_WAIT200)
static void devtest_first_cleanup_task(void *arg)
{
    (void)arg;
    s_first_cleanup_started = true;
    s_first_cleanup_result = app_cmd_net_cleanup_run(s_op, NULL);
    xEventGroupSetBits(s_events, EV_FIRST_CLEANUP_DONE);
    vTaskDelete(NULL);
}

#endif

#if defined(APP_DEV_TEST_SCENARIO_T13) || defined(APP_DEV_TEST_SCENARIO_WAIT200)
static bool devtest_start_claim_task(TaskFunction_t fn)
{
    s_claim_task = xTaskCreateStatic(fn, "devtest_claim",
                                     DEVTEST_TASK_STACK_DEPTH, NULL,
                                     DEVTEST_TASK_PRIO,
                                     s_claim_stack, &s_claim_tcb);
    return s_claim_task != NULL;
}

#endif

static bool devtest_do_finish(const char *reason)
{
    app_operation_status_t st;

    if (!app_operation_get_status(s_op, &st)) {
        ESP_LOGE(TAG, "DEVTEST_FINISH status lost reason=%s", reason);
        return false;
    }
    s_finish_state = DEV_FINISH_CALL_STARTED;
    xEventGroupSetBits(s_events, EV_FINISH_START);
    ESP_LOGI(TAG, "DEVTEST_FINISH request reason=%s op=%s", reason, s_op_id);
    s_finish_result = app_operation_request_finish(
        s_op, s_generation, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    s_finish_result_valid = true;
    s_finish_state = DEV_FINISH_RETURNED;
    xEventGroupSetBits(s_events, EV_FINISH_DONE);
    ESP_LOGI(TAG, "DEVTEST_FINISH result=%d op=%s", (int)s_finish_result, s_op_id);
    if (s_finish_result == APP_OPERATION_FINISH_PENDING) {
        app_operation_cleanup_state_t c;
        if (!devtest_wait_claim_settled(&c, DEVTEST_TERMINAL_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "DEVTEST_FINISH pending claim timeout op=%s", s_op_id);
            return false;
        }
        return c == APP_OPERATION_CLEANUP_OK;
    }
    return s_finish_result == APP_OPERATION_FINISH_DONE;
}

static bool devtest_expect_cancelled(const char *tag)
{
    app_operation_status_t st;

    if (!app_operation_get_status(s_op, &st)) {
        ESP_LOGE(TAG, "DEVTEST_STATE %s status unavailable op=%s", tag, s_op_id);
        return false;
    }
    if (st.generation != s_generation) {
        ESP_LOGE(TAG, "DEVTEST_IDENTITY_ERROR %s gen=%u expected=%u op=%s",
                 tag, (unsigned)st.generation, (unsigned)s_generation, s_op_id);
        return false;
    }
    if (st.state != APP_OP_CANCELLED || st.error != APP_OPERATION_ERR_CANCELLED ||
        st.cleanup_state != APP_OPERATION_CLEANUP_OK) {
        ESP_LOGE(TAG, "DEVTEST_STATE %s unexpected state=%d error=%d cleanup=%d op=%s",
                 tag, (int)st.state, (int)st.error, (int)st.cleanup_state, s_op_id);
        return false;
    }
    ESP_LOGI(TAG, "DEVTEST_STATE %s state=cancelled cleanup=ok op=%s", tag, s_op_id);
    return true;
}

static bool devtest_check_resources(void)
{
    app_cmd_net_job_snap_t job;
    app_cmd_net_trial_snap_t trial;
    app_operation_status_t st;
    bool gen_ok;

    app_cmd_net_job_lock();
    app_cmd_net_job_snapshot_locked(&job);
    app_cmd_net_trial_snapshot_locked(&trial);
    gen_ok = app_operation_generation_matches(s_op, s_generation);
    app_cmd_net_job_unlock();

    if (!app_operation_get_status(s_op, &st)) {
        return false;
    }
    if (job.active) {
        ESP_LOGE(TAG, "DEVTEST_RESOURCE job_active=1 op=%s", s_op_id);
        return false;
    }
    if (trial.valid) {
        ESP_LOGE(TAG, "DEVTEST_RESOURCE trial_valid=1 op=%s", s_op_id);
        return false;
    }
    if (!gen_ok || st.generation != s_generation) {
        ESP_LOGE(TAG, "DEVTEST_RESOURCE generation mismatch op=%s", s_op_id);
        return false;
    }
    return true;
}

static bool devtest_success_or_cleanup(const char *reason, bool scenario_ok)
{
    bool cleanup_ok = true;
    bool first_cleanup_failed = false;
    app_operation_cleanup_state_t claim = APP_OPERATION_CLEANUP_NONE;
    app_operation_status_t st = {0};

    devtest_release_barrier();

    /* WAIT200 的第一 claimant 必须先退出；失败时不得再发起 finish。 */
    if (s_first_cleanup_task_created || s_first_cleanup_started) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_FIRST_CLEANUP_DONE,
                                               pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
        if ((bits & EV_FIRST_CLEANUP_DONE) == 0 || !s_first_cleanup_result) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP first cleanup failed op=%s", s_op_id);
            cleanup_ok = false;
            first_cleanup_failed = true;
        }
    }

    /* T13 的 first finish 已发起但尚未返回：只等待，不重复发起。 */
    if (s_finish_task_created && s_finish_state != DEV_FINISH_RETURNED) {
        EventBits_t bits = xEventGroupWaitBits(s_events, EV_FINISH_DONE, pdFALSE, pdTRUE,
                                               pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
        if ((bits & EV_FINISH_DONE) == 0) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP first finish timeout op=%s", s_op_id);
            cleanup_ok = false;
        }
    }
    if (s_finish_task_created && s_finish_result_valid &&
        s_finish_result != APP_OPERATION_FINISH_DONE &&
        s_finish_result != APP_OPERATION_FINISH_PENDING) {
        ESP_LOGE(TAG, "DEVTEST_CLEANUP first finish result=%d op=%s",
                 (int)s_finish_result, s_op_id);
        cleanup_ok = false;
    }

    if (cleanup_ok && !first_cleanup_failed && !s_finish_task_created &&
        s_finish_state == DEV_FINISH_NOT_STARTED) {
        if (!devtest_do_finish(reason)) {
            cleanup_ok = false;
        }
    } else if (cleanup_ok && s_finish_state == DEV_FINISH_RETURNED &&
               s_finish_result_valid &&
               s_finish_result == APP_OPERATION_FINISH_PENDING) {
        if (!devtest_wait_claim_settled(&claim, DEVTEST_TERMINAL_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP pending claim timeout op=%s", s_op_id);
            cleanup_ok = false;
        }
    }

    devtest_release_run();
    if (s_run_started && !devtest_wait_run_exit()) {
        ESP_LOGE(TAG, "DEVTEST_CLEANUP run exit timeout op=%s", s_op_id);
        cleanup_ok = false;
    }

    /* 冻结规则：先看 cleanup claim。FAIL 直接失败，不等待成功终态。 */
    if (cleanup_ok) {
        if (!devtest_wait_claim_settled(&claim, DEVTEST_TERMINAL_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP claim timeout op=%s", s_op_id);
            cleanup_ok = false;
        } else if (claim != APP_OPERATION_CLEANUP_OK) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP claim=%d op=%s", (int)claim, s_op_id);
            cleanup_ok = false;
        }
    }

    if (cleanup_ok) {
        if (!devtest_wait_status_terminal(DEVTEST_TERMINAL_TIMEOUT_MS)) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP terminal timeout op=%s", s_op_id);
            cleanup_ok = false;
        }
        if (cleanup_ok && !devtest_check_resources()) {
            cleanup_ok = false;
        }
        if (cleanup_ok && app_operation_get_status(s_op, &st)) {
            ESP_LOGI(TAG, "DEVTEST_FINAL state=%d error=%d cleanup=%d op=%s",
                     (int)st.state, (int)st.error, (int)st.cleanup_state, s_op_id);
            vTaskDelay(pdMS_TO_TICKS(DEVTEST_WORKER_SETTLE_MS));
            app_operation_status_t st_after = {0};
            bool got_after = app_operation_get_status(s_op, &st_after);
            bool job_active_after = app_cmd_net_job_is_active();
            bool stable = got_after && st_after.generation == s_generation &&
                          st_after.state == st.state &&
                          st_after.cleanup_state == APP_OPERATION_CLEANUP_OK &&
                          !job_active_after;
            ESP_LOGI(TAG, "DEVTEST_WORKER_SETTLE settle_ms=%u terminal_before=%d "
                          "terminal_after=%d job_active_after=%d "
                          "running_observed=unknown evidence=weak",
                     (unsigned)DEVTEST_WORKER_SETTLE_MS, (int)st.state,
                     (int)st_after.state, (int)job_active_after);
            if (!stable) {
                ESP_LOGE(TAG, "DEVTEST_CLEANUP settle check failed op=%s", s_op_id);
                cleanup_ok = false;
            }
        } else if (cleanup_ok) {
            ESP_LOGE(TAG, "DEVTEST_CLEANUP final status unavailable op=%s", s_op_id);
            cleanup_ok = false;
        }
    } else {
        if (app_operation_get_status(s_op, &st)) {
            ESP_LOGE(TAG, "DEVTEST_FINAL failure state=%d error=%d cleanup=%d op=%s",
                     (int)st.state, (int)st.error, (int)st.cleanup_state, s_op_id);
        }
    }

    return scenario_ok && cleanup_ok;
}

#if defined(APP_DEV_TEST_SCENARIO_T13)
static bool devtest_run_t13(void)
{
    EventBits_t bits;
    app_operation_finish_result_t second;
    app_operation_cleanup_state_t claim = APP_OPERATION_CLEANUP_NONE;

    if (!devtest_start_claim_task(devtest_finish_task)) {
        ESP_LOGE(TAG, "DEVTEST_T13 claim task create failed");
        return false;
    }
    s_finish_task_created = true;
    bits = xEventGroupWaitBits(s_events, EV_BARRIER_HIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
    if ((bits & EV_BARRIER_HIT) == 0) {
        ESP_LOGE(TAG, "DEVTEST_T13 barrier timeout op=%s", s_op_id);
        return false;
    }
    ESP_LOGI(TAG, "DEVTEST_T13_FIRST_FINISH_REGISTERED_OBSERVED_AT_CLAIM op=%s",
             s_op_id);

    second = app_operation_request_finish(s_op, s_generation, APP_OP_CANCELLED,
                                          APP_OPERATION_ERR_CANCELLED,
                                          APP_OPERATION_CLEANUP_REQUIRED);
    ESP_LOGI(TAG, "DEVTEST_T13_SECOND_FINISH result=%d op=%s", (int)second, s_op_id);
    if (second != APP_OPERATION_FINISH_PENDING) {
        return false;
    }

    devtest_release_barrier();
    bits = xEventGroupWaitBits(s_events, EV_FINISH_DONE, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
    if ((bits & EV_FINISH_DONE) == 0 || !s_finish_result_valid ||
        s_finish_result != APP_OPERATION_FINISH_DONE) {
        ESP_LOGE(TAG, "DEVTEST_T13 first finish not DONE op=%s", s_op_id);
        return false;
    }
    if (!devtest_wait_claim_settled(&claim, DEVTEST_TERMINAL_TIMEOUT_MS) ||
        claim != APP_OPERATION_CLEANUP_OK) {
        ESP_LOGE(TAG, "DEVTEST_T13 cleanup claim failed op=%s", s_op_id);
        return false;
    }
    if (app_cmd_net_job_dev_test_claim_count() != 1) {
        ESP_LOGE(TAG, "DEVTEST_T13 claim_count=%u op=%s",
                 (unsigned)app_cmd_net_job_dev_test_claim_count(), s_op_id);
        return false;
    }
    if (app_cmd_net_job_dev_test_restore_request_count() != 1 ||
        !app_cmd_net_job_dev_test_last_restore_ok()) {
        ESP_LOGE(TAG, "DEVTEST_T13 restore_count=%u ok=%d op=%s",
                 (unsigned)app_cmd_net_job_dev_test_restore_request_count(),
                 (int)app_cmd_net_job_dev_test_last_restore_ok(), s_op_id);
        return false;
    }
    return devtest_expect_cancelled("t13");
}

#endif

#if defined(APP_DEV_TEST_SCENARIO_T19)
static bool devtest_run_t19(void)
{
    app_cmd_net_job_snap_t job;
    app_cmd_net_trial_snap_t trial;
    app_operation_finish_result_t finish;
    app_operation_cleanup_state_t claim = APP_OPERATION_CLEANUP_NONE;
    bool gen_ok;

    if (app_cmd_net_trial_store("devtest-ssid", "devtest-pass",
                                APP_CMD_SOURCE_SERIAL, s_op_id, NULL, s_op, NULL) !=
        APP_CMD_NET_TRIAL_STORED) {
        ESP_LOGE(TAG, "DEVTEST_T19 trial store failed op=%s", s_op_id);
        return false;
    }
    ESP_LOGI(TAG, "DEVTEST_T19_TRIAL_STORED op=%s", s_op_id);

    app_cmd_net_job_lock();
    app_cmd_net_job_clear_locked();
    app_cmd_net_job_snapshot_locked(&job);
    app_cmd_net_trial_snapshot_locked(&trial);
    gen_ok = app_operation_generation_matches(s_op, s_generation);
    app_cmd_net_job_unlock();

    ESP_LOGI(TAG, "DEVTEST_T19_JOB_CLEARED job_active=%d trial_valid=%d "
                  "trial_op=%s op=%s gen_ok=%d",
             (int)job.active, (int)trial.valid, trial.operation_id, s_op_id,
             (int)gen_ok);
    if (job.active || !trial.valid || strcmp(trial.operation_id, s_op_id) != 0 ||
        !gen_ok) {
        ESP_LOGE(TAG, "DEVTEST_T19 trial_only precondition failed op=%s", s_op_id);
        return false;
    }

    finish = app_operation_request_finish(s_op, s_generation, APP_OP_CANCELLED,
                                          APP_OPERATION_ERR_CANCELLED,
                                          APP_OPERATION_CLEANUP_REQUIRED);
    s_finish_result = finish;
    s_finish_result_valid = true;
    s_finish_state = DEV_FINISH_RETURNED;
    ESP_LOGI(TAG, "DEVTEST_T19_REQUIRED_FINISH result=%d op=%s", (int)finish, s_op_id);
    if (finish != APP_OPERATION_FINISH_DONE) {
        if (!devtest_wait_claim_settled(&claim, DEVTEST_TERMINAL_TIMEOUT_MS) ||
            claim != APP_OPERATION_CLEANUP_OK) {
            return false;
        }
    }
    if (!devtest_wait_claim_settled(&claim, DEVTEST_TERMINAL_TIMEOUT_MS) ||
        claim != APP_OPERATION_CLEANUP_OK) {
        ESP_LOGE(TAG, "DEVTEST_T19 claim failed op=%s", s_op_id);
        return false;
    }
    if (app_cmd_net_job_dev_test_claim_count() != 1 ||
        app_cmd_net_job_dev_test_restore_request_count() != 1 ||
        !app_cmd_net_job_dev_test_last_restore_ok()) {
        ESP_LOGE(TAG, "DEVTEST_T19 counters claim=%u restore=%u ok=%d op=%s",
                 (unsigned)app_cmd_net_job_dev_test_claim_count(),
                 (unsigned)app_cmd_net_job_dev_test_restore_request_count(),
                 (int)app_cmd_net_job_dev_test_last_restore_ok(), s_op_id);
        return false;
    }
    return devtest_expect_cancelled("t19");
}

#endif

#if defined(APP_DEV_TEST_SCENARIO_WAIT200)
static bool devtest_run_wait200(void)
{
    app_operation_cleanup_state_t claim = APP_OPERATION_CLEANUP_NONE;
    EventBits_t bits;
    bool second_result;
    int64_t start_us;
    int64_t end_us;
    uint32_t waited_ticks;
    uint32_t limit_ticks;
    app_operation_cleanup_state_t claim_at_return;

    if (!devtest_start_claim_task(devtest_first_cleanup_task)) {
        ESP_LOGE(TAG, "DEVTEST_WAIT task create failed");
        return false;
    }
    s_first_cleanup_task_created = true;
    bits = xEventGroupWaitBits(s_events, EV_BARRIER_HIT, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
    if ((bits & EV_BARRIER_HIT) == 0) {
        ESP_LOGE(TAG, "DEVTEST_WAIT barrier timeout op=%s", s_op_id);
        return false;
    }
    ESP_LOGI(TAG, "DEVTEST_WAIT_START op=%s wait_ms=200 tick_hz=%u",
             s_op_id, (unsigned)configTICK_RATE_HZ);

    second_result = app_cmd_net_cleanup_run(s_op, NULL);

    start_us = app_cmd_net_job_dev_test_wait_start_us();
    end_us = app_cmd_net_job_dev_test_wait_end_us();
    waited_ticks = app_cmd_net_job_dev_test_wait_waited_ticks();
    limit_ticks = app_cmd_net_job_dev_test_wait_limit_ticks();
    claim_at_return = app_cmd_net_job_dev_test_wait_claim_at_return();
    ESP_LOGI(TAG, "DEVTEST_WAIT_END op=%s second_result=%d elapsed_us=%lld "
                  "elapsed_ms=%lld limit_ticks=%u waited_ticks=%u claim=%d",
             s_op_id, (int)second_result,
             (long long)(end_us - start_us),
             (long long)((end_us - start_us) / 1000),
             (unsigned)limit_ticks, (unsigned)waited_ticks,
             (int)claim_at_return);
    if (second_result || claim_at_return != APP_OPERATION_CLEANUP_RUNNING ||
        end_us <= start_us || limit_ticks == 0) {
        ESP_LOGE(TAG, "DEVTEST_WAIT unexpected second result op=%s", s_op_id);
        return false;
    }

    devtest_release_barrier();
    bits = xEventGroupWaitBits(s_events, EV_FIRST_CLEANUP_DONE, pdFALSE, pdTRUE,
                               pdMS_TO_TICKS(DEVTEST_FINISH_TIMEOUT_MS));
    if ((bits & EV_FIRST_CLEANUP_DONE) == 0 || !s_first_cleanup_result) {
        ESP_LOGE(TAG, "DEVTEST_WAIT first cleanup failed op=%s", s_op_id);
        return false;
    }
    if (!devtest_wait_claim_settled(&claim, DEVTEST_TERMINAL_TIMEOUT_MS) ||
        claim != APP_OPERATION_CLEANUP_OK) {
        ESP_LOGE(TAG, "DEVTEST_WAIT final claim failed op=%s", s_op_id);
        return false;
    }
    ESP_LOGI(TAG, "DEVTEST_WAIT_FIRST_CLEANUP_DONE op=%s claim=%d",
             s_op_id, (int)claim);

    if (!devtest_do_finish("wait200_final")) {
        ESP_LOGE(TAG, "DEVTEST_WAIT final finish failed op=%s", s_op_id);
        return false;
    }
    if (app_cmd_net_job_dev_test_claim_count() != 1 ||
        app_cmd_net_job_dev_test_restore_request_count() != 1 ||
        !app_cmd_net_job_dev_test_last_restore_ok()) {
        ESP_LOGE(TAG, "DEVTEST_WAIT counters claim=%u restore=%u ok=%d op=%s",
                 (unsigned)app_cmd_net_job_dev_test_claim_count(),
                 (unsigned)app_cmd_net_job_dev_test_restore_request_count(),
                 (int)app_cmd_net_job_dev_test_last_restore_ok(), s_op_id);
        return false;
    }
    return devtest_expect_cancelled("wait200");
}

#endif

static void devtest_control_task(void *arg)
{
    bool scenario_ok = false;
    bool cleanup_ok = false;
    const char *outcome = "FAIL";
    app_operation_status_t st = {0};

    (void)arg;
    snprintf(s_run_id, sizeof(s_run_id), "%08X%08X",
             (unsigned)esp_random(), (unsigned)esp_timer_get_time());
    ESP_LOGI(TAG, "DEVTEST_BOOT run_id=%s scenario=%s build_sha=%s fw=%s",
             s_run_id, DEVTEST_SCENARIO_NAME, esp_app_get_elf_sha256_str(),
             esp_app_get_description() != NULL ? esp_app_get_description()->version : "");
    app_cmd_net_job_dev_test_obs_reset();

    if (!devtest_wait_net_ready()) {
        ESP_LOGW(TAG, "DEVTEST_RESULT run_id=%s scenario=%s outcome=SKIP "
                      "reason=net_not_ready", s_run_id, DEVTEST_SCENARIO_NAME);
        vTaskDelete(NULL);
        return;
    }
    if (!devtest_begin_operation()) {
        ESP_LOGW(TAG, "DEVTEST_RESULT run_id=%s scenario=%s outcome=%s reason=%s",
                 s_run_id, DEVTEST_SCENARIO_NAME,
                 s_admission_skip ? "SKIP" : "FAIL", s_admission_reason);
        vTaskDelete(NULL);
        return;
    }
    /* Admission Gate 成功且测试 operation 身份已保存后才注册 hook。 */
    app_cmd_net_job_dev_test_hook_set(devtest_hook);
    if (!devtest_wait_connecting()) {
        cleanup_ok = devtest_success_or_cleanup("run_not_connecting", false);
        ESP_LOGE(TAG, "DEVTEST_RESULT run_id=%s scenario=%s outcome=FAIL "
                      "reason=run_not_connecting cleanup_ok=%d",
                 s_run_id, DEVTEST_SCENARIO_NAME, (int)cleanup_ok);
        vTaskDelete(NULL);
        return;
    }

    if (!devtest_net_ready()) {
        ESP_LOGW(TAG, "DEVTEST_RESULT run_id=%s scenario=%s outcome=SKIP_PENDING "
                      "reason=net_not_ready_after_admission", s_run_id,
                 DEVTEST_SCENARIO_NAME);
        cleanup_ok = devtest_success_or_cleanup("net_not_ready_after_admission", false);
        ESP_LOGI(TAG, "DEVTEST_RESULT run_id=%s scenario=%s outcome=%s "
                      "reason=net_not_ready_after_admission",
                 s_run_id, DEVTEST_SCENARIO_NAME, cleanup_ok ? "SKIP" : "FAIL");
        vTaskDelete(NULL);
        return;
    }

#if defined(APP_DEV_TEST_SCENARIO_T13)
    scenario_ok = devtest_run_t13();
#elif defined(APP_DEV_TEST_SCENARIO_T19)
    scenario_ok = devtest_run_t19();
#elif defined(APP_DEV_TEST_SCENARIO_WAIT200)
    scenario_ok = devtest_run_wait200();
#endif

    cleanup_ok = devtest_success_or_cleanup("scenario_end", scenario_ok);
    if (app_operation_get_status(s_op, &st)) {
        ESP_LOGI(TAG, "DEVTEST_COUNTERS claim=%u restore=%u restore_ok=%d "
                      "state=%d cleanup=%d",
                 (unsigned)app_cmd_net_job_dev_test_claim_count(),
                 (unsigned)app_cmd_net_job_dev_test_restore_request_count(),
                 (int)app_cmd_net_job_dev_test_last_restore_ok(),
                 (int)st.state, (int)st.cleanup_state);
    }
    outcome = (scenario_ok && cleanup_ok) ? "PASS" : "FAIL";
    ESP_LOGI(TAG, "DEVTEST_RESULT run_id=%s scenario=%s outcome=%s "
                  "acceptance=%s", s_run_id, DEVTEST_SCENARIO_NAME, outcome,
             strcmp(DEVTEST_SCENARIO_NAME, "wait200") == 0 ? "pilot_pending" : "final");
    vTaskDelete(NULL);
}

esp_err_t app_dev_test_helper_start(void)
{
    if (s_started) {
        return ESP_ERR_INVALID_STATE;
    }
    s_events = xEventGroupCreateStatic(&s_events_buf);
    if (s_events == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_control_task = xTaskCreateStatic(devtest_control_task, "devtest",
                                       DEVTEST_TASK_STACK_DEPTH, NULL,
                                       DEVTEST_TASK_PRIO,
                                       s_control_stack, &s_control_tcb);
    if (s_control_task == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_started = true;
    return ESP_OK;
}

#endif /* APP_DEV_TEST_HELPER */
