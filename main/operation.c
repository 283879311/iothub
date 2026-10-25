#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_err.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bt_gatts.h"
#include "mqtt.h"
#include "operation.h"

static const char *TAG = "operation";

#define APP_OPERATION_DEFAULT_ACCEPTED_TTL_MS 20000
#define APP_OPERATION_DEFAULT_GOT_IP_TTL_MS 300000
/* 终态保留统一 600s（幂等窗口：同 key 重试在保留期内返回原终态，契约见
 * G3 操作契约冻结/检查表第 15 项）。槽满由"覆盖最老终态"解决，不再靠
 * 缩短保留换可用性——淘汰上线后分级保留失去动机，统一保留让契约变更
 * 收敛为唯一一条行为差异。 */
#define APP_OPERATION_DEFAULT_RETENTION_MS 600000
#define APP_OPERATION_WORKER_STACK 4096

struct app_operation {
    bool used;
    bool expiring;
    uint8_t generation;
    app_operation_owner_t owner;
    app_op_state_t state;
    app_operation_error_t error;
    char id[APP_OPERATION_ID_LEN];
    char idem_key[APP_OPERATION_IDEM_KEY_LEN];
    int64_t created_ms;
    int64_t deadline_ms;
    int64_t got_ip_ttl_ms;
    int64_t terminal_ms;
    app_operation_run_fn run_fn;
    app_operation_cleanup_fn cleanup_fn;
    void *ctx;
};

static app_operation_t s_ops[APP_OPERATION_MAX_SLOTS];
static portMUX_TYPE s_ops_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_worker = NULL;
static bool s_worker_started = false;
static bool s_initialized = false;
static uint32_t s_retention_ms = APP_OPERATION_DEFAULT_RETENTION_MS;

/* 注意单位：ESP-IDF 的 xTaskCreate() 以字节计，但 xTaskCreateStatic() 最终进入
 * prvInitialiseNewTask(uxStackDepth)，按 StackType_t 元素数解释。静态 buffer 要
 * 同时按元素数分配，才能保持与原 4096B 动态栈相同的栈大小。 */
#define APP_OPERATION_WORKER_STACK_DEPTH (APP_OPERATION_WORKER_STACK / sizeof(StackType_t))
#if defined(APP_OPERATION_WORKER_STATIC) && APP_OPERATION_WORKER_STATIC
#if (configSUPPORT_STATIC_ALLOCATION != 1)
#error "APP_OPERATION_WORKER_STATIC requires configSUPPORT_STATIC_ALLOCATION"
#endif
static StackType_t s_worker_stack[APP_OPERATION_WORKER_STACK_DEPTH];
static StaticTask_t s_worker_tcb;
#endif

static int64_t app_operation_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool app_operation_valid(const app_operation_t *op)
{
    return op != NULL && op >= &s_ops[0] && op < &s_ops[APP_OPERATION_MAX_SLOTS] &&
           op->used;
}

app_operation_t *app_operation_find(const char *operation_id)
{
    app_operation_t *found = NULL;

    if (operation_id == NULL || operation_id[0] == '\0') {
        return NULL;
    }
    portENTER_CRITICAL(&s_ops_mux);
    for (size_t i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (s_ops[i].used && strcmp(s_ops[i].id, operation_id) == 0) {
            found = &s_ops[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return found;
}

app_operation_t *app_operation_find_by_key(app_operation_owner_t owner, const char *idem_key)
{
    app_operation_t *found = NULL;

    if (idem_key == NULL || idem_key[0] == '\0') {
        return NULL;
    }
    portENTER_CRITICAL(&s_ops_mux);
    for (size_t i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (s_ops[i].used && s_ops[i].owner == owner &&
            strcmp(s_ops[i].idem_key, idem_key) == 0) {
            found = &s_ops[i];
            break;
        }
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return found;
}

const char *app_operation_id(const app_operation_t *op)
{
    return app_operation_valid(op) ? op->id : "";
}

app_op_state_t app_operation_state(const app_operation_t *op)
{
    return app_operation_valid(op) ? op->state : APP_OP_FREE;
}

const char *app_operation_error_name(app_operation_error_t error)
{
    switch (error) {
    case APP_OPERATION_ERR_NONE:           return "none";
    case APP_OPERATION_ERR_BUSY:           return "busy";
    case APP_OPERATION_ERR_NO_SLOT:        return "no_slot";
    case APP_OPERATION_ERR_INTERNAL:       return "internal_error";
    case APP_OPERATION_ERR_CLEANUP_FAILED: return "cleanup_failed";
    case APP_OPERATION_ERR_TIMEOUT:        return "timeout";
    case APP_OPERATION_ERR_CANCELLED:      return "cancelled";
    case APP_OPERATION_ERR_NOT_FOUND:      return "not_found";
    case APP_OPERATION_ERR_WRONG_PASSWORD: return "wrong_password";
    case APP_OPERATION_ERR_AP_NOT_FOUND:   return "ap_not_found";
    case APP_OPERATION_ERR_CONNECT_FAILED: return "connect_failed";
    case APP_OPERATION_ERR_TRIAL_INVALID:  return "trial_invalid";
    case APP_OPERATION_ERR_OPERATION_STATE:return "operation_state";
    case APP_OPERATION_ERR_STORAGE_FAILED: return "storage_failed";
    case APP_OPERATION_ERR_ROLLBACK_FAILED:return "rollback_failed";
    case APP_OPERATION_ERR_SCAN_FAILED:   return "wifi_scan_failed";
    default:                               return "unknown";
    }
}

static void app_operation_fill_id(char *out, size_t out_size)
{
    snprintf(out, out_size, "op-%08lX%08lX",
             (unsigned long)esp_random(), (unsigned long)esp_random());
}

static bool app_operation_has_active_locked(void)
{
    size_t i;

    for (i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (s_ops[i].used && app_op_state_is_active(s_ops[i].state)) {
            return true;
        }
    }
    return false;
}

static void app_operation_reclaim_terminal_locked(int64_t now_ms)
{
    size_t i;

    for (i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        app_operation_t *op = &s_ops[i];

        if (!op->used || !app_op_state_is_terminal(op->state)) {
            continue;
        }
        if (now_ms - op->terminal_ms < (int64_t)s_retention_ms) {
            continue;
        }
        op->state = APP_OP_FREE;
        op->used = false;
    }
}

/* 槽满且无活动 operation 时的兜底：回收最老的终态记录，让新请求总有人接待。
 * 只在确认全表为终态时调用（active 已被上层 busy 分支拦截）。 */
static app_operation_t *app_operation_oldest_terminal_locked(void)
{
    app_operation_t *oldest = NULL;
    size_t i;

    for (i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        app_operation_t *op = &s_ops[i];

        if (!op->used || !app_op_state_is_terminal(op->state)) {
            continue;
        }
        if (oldest == NULL || op->terminal_ms < oldest->terminal_ms) {
            oldest = op;
        }
    }
    return oldest;
}

static void app_operation_worker(void *arg);
static esp_err_t app_operation_ensure_worker(void);

esp_err_t app_operation_init(void)
{
    if (s_initialized) {
        return ESP_OK;
    }
    memset(s_ops, 0, sizeof(s_ops));
    s_initialized = true;
    ESP_LOGI(TAG, "operation table ready: slots=%u", (unsigned)APP_OPERATION_MAX_SLOTS);
    return ESP_OK;
}

esp_err_t app_operation_start(app_operation_owner_t owner, const char *idem_key,
                              uint32_t accepted_ttl_ms, uint32_t got_ip_ttl_ms,
                              app_operation_run_fn run_fn,
                              app_operation_cleanup_fn cleanup_fn,
                              void *ctx, app_operation_t **out)
{
    int64_t now_ms;
    app_operation_t *slot = NULL;
    size_t i;
    bool defer_started = false;

    if (out != NULL) {
        *out = NULL;
    }
    if (!s_initialized || run_fn == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (accepted_ttl_ms == 0) {
        accepted_ttl_ms = APP_OPERATION_DEFAULT_ACCEPTED_TTL_MS;
    }
    if (got_ip_ttl_ms == 0) {
        got_ip_ttl_ms = APP_OPERATION_DEFAULT_GOT_IP_TTL_MS;
    }

    /* 网络 operation 开始前先与 MQTT rebuild 协调；如已有 rebuild 在跑则
     * 明确返回 busy，不能先受理再面对并发峰值。内部测试 operation 不参与。 */
    if (owner != APP_OPERATION_OWNER_INTERNAL) {
        esp_err_t defer_err = app_mqtt_defer_restart_begin();
        if (defer_err != ESP_OK) {
            return defer_err;
        }
        defer_started = true;
    }

    now_ms = app_operation_now_ms();
    char reclaimed_id[APP_OPERATION_ID_LEN] = "";
    app_op_state_t reclaimed_state = APP_OP_FREE;
    int64_t reclaimed_age_ms = -1;
    portENTER_CRITICAL(&s_ops_mux);
    app_operation_reclaim_terminal_locked(now_ms);
    if (app_operation_has_active_locked()) {
        portEXIT_CRITICAL(&s_ops_mux);
        if (defer_started) {
            app_mqtt_defer_restart_end();
        }
        return ESP_ERR_INVALID_STATE;   /* busy */
    }
    for (i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (!s_ops[i].used) {
            slot = &s_ops[i];
            break;
        }
    }
    if (slot == NULL) {
        /* 全表为终态保留记录：覆盖最老的一条而不是拒绝新请求。
         * no_slot 从此只在极端路径（如 worker 创建失败前的竞态）出现。 */
        slot = app_operation_oldest_terminal_locked();
        if (slot == NULL) {
            portEXIT_CRITICAL(&s_ops_mux);
            if (defer_started) {
                app_mqtt_defer_restart_end();
            }
            return ESP_ERR_NO_MEM;      /* 理论不可达：无活动且无终态却无空槽 */
        }
        /* 临界区内只取证，日志在锁外打 */
        snprintf(reclaimed_id, sizeof(reclaimed_id), "%s", slot->id);
        reclaimed_state = slot->state;
        reclaimed_age_ms = now_ms - slot->terminal_ms;
    }

    memset(slot, 0, sizeof(*slot));
    slot->used = true;
    slot->generation++;
    slot->owner = owner;
    slot->state = APP_OP_ACCEPTED;
    slot->error = APP_OPERATION_ERR_NONE;
    slot->created_ms = now_ms;
    slot->deadline_ms = now_ms + (int64_t)accepted_ttl_ms;
    slot->got_ip_ttl_ms = got_ip_ttl_ms;
    slot->run_fn = run_fn;
    slot->cleanup_fn = cleanup_fn;
    slot->ctx = ctx;
    app_operation_fill_id(slot->id, sizeof(slot->id));
    if (idem_key != NULL) {
        snprintf(slot->idem_key, sizeof(slot->idem_key), "%s", idem_key);
    }
    portEXIT_CRITICAL(&s_ops_mux);

    if (reclaimed_age_ms >= 0) {
        ESP_LOGW(TAG, "reclaim oldest terminal %s state=%s age=%ums for new request",
                 reclaimed_id, app_op_state_name(reclaimed_state),
                 (unsigned)reclaimed_age_ms);
    }

    if (app_operation_ensure_worker() != ESP_OK) {
        portENTER_CRITICAL(&s_ops_mux);
        slot->used = false;
        slot->state = APP_OP_FREE;
        portEXIT_CRITICAL(&s_ops_mux);
        if (defer_started) {
            app_mqtt_defer_restart_end();
        }
        return ESP_ERR_NO_MEM;
    }

    ESP_LOGI(TAG, "accepted %s owner=%d ttl=%ums got_ip_ttl=%ums",
             slot->id, (int)owner, (unsigned)accepted_ttl_ms,
             (unsigned)got_ip_ttl_ms);
    xTaskNotifyGive(s_worker);
    if (out != NULL) {
        *out = slot;
    }
    return ESP_OK;
}

esp_err_t app_operation_set_state(app_operation_t *op, app_op_state_t state,
                                  app_operation_error_t error)
{
    int64_t now_ms;
    bool terminal;

    if (!app_operation_valid(op)) {
        return ESP_ERR_INVALID_ARG;
    }
    now_ms = app_operation_now_ms();
    terminal = app_op_state_is_terminal(state);

    portENTER_CRITICAL(&s_ops_mux);
    if (!app_operation_valid(op)) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_ARG;
    }
    if (state == APP_OP_FREE || !app_op_transition_allowed(op->state, state)) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_STATE;
    }
    op->state = state;
    op->error = error;
    if (state == APP_OP_GOT_IP) {
        op->deadline_ms = now_ms + op->got_ip_ttl_ms;
    } else if (state == APP_OP_COMMITTING) {
        op->deadline_ms = 0;   /* committing 不可取消、不按候选 TTL 过期 */
    } else if (terminal) {
        op->deadline_ms = 0;
        op->terminal_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    if (terminal) {
        app_mqtt_defer_restart_end();
    }
    return ESP_OK;
}

esp_err_t app_operation_cancel(app_operation_t *op)
{
    app_operation_cleanup_fn cleanup = NULL;
    void *ctx = NULL;
    uint8_t generation;
    bool need_cleanup = false;
    bool terminal = false;

    if (!app_operation_valid(op)) {
        return ESP_ERR_INVALID_ARG;
    }

    portENTER_CRITICAL(&s_ops_mux);
    if (!app_operation_valid(op)) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_ARG;
    }
    if (!app_op_state_can_cancel(op->state)) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_STATE;
    }
    if (op->expiring) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_STATE;
    }
    op->expiring = true;
    generation = op->generation;
    cleanup = op->cleanup_fn;
    ctx = op->ctx;
    need_cleanup = (cleanup != NULL);
    portEXIT_CRITICAL(&s_ops_mux);

    if (need_cleanup && !cleanup(op, ctx)) {
        portENTER_CRITICAL(&s_ops_mux);
        if (op->used && op->generation == generation) {
            op->state = APP_OP_FAILED;
            op->error = APP_OPERATION_ERR_CLEANUP_FAILED;
            op->deadline_ms = 0;
            op->terminal_ms = app_operation_now_ms();
            op->expiring = false;
            terminal = true;
        }
        portEXIT_CRITICAL(&s_ops_mux);
        if (terminal) {
            app_mqtt_defer_restart_end();
        }
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_ops_mux);
    if (op->used && op->generation == generation && app_op_state_is_active(op->state)) {
        op->state = APP_OP_CANCELLED;
        op->error = APP_OPERATION_ERR_CANCELLED;
        op->deadline_ms = 0;
        op->terminal_ms = app_operation_now_ms();
        terminal = true;
    }
    op->expiring = false;
    portEXIT_CRITICAL(&s_ops_mux);
    if (terminal) {
        app_mqtt_defer_restart_end();
    }
    return ESP_OK;
}

bool app_operation_get_status(const app_operation_t *op, app_operation_status_t *out)
{
    bool ok = false;

    if (out == NULL || !app_operation_valid(op)) {
        return false;
    }
    portENTER_CRITICAL(&s_ops_mux);
    if (app_operation_valid(op)) {
        memset(out, 0, sizeof(*out));
        memcpy(out->id, op->id, sizeof(out->id));
        memcpy(out->idem_key, op->idem_key, sizeof(out->idem_key));
        out->owner = op->owner;
        out->state = op->state;
        out->error = op->error;
        out->created_ms = op->created_ms;
        out->deadline_ms = op->deadline_ms;
        out->terminal_ms = op->terminal_ms;
        out->generation = op->generation;
        ok = true;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return ok;
}

static void app_operation_run_pending(void)
{
    app_operation_t *op = NULL;
    app_operation_run_fn run_fn = NULL;
    void *ctx = NULL;
    esp_err_t err;

    portENTER_CRITICAL(&s_ops_mux);
    for (size_t i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (s_ops[i].used && s_ops[i].state == APP_OP_ACCEPTED && !s_ops[i].expiring) {
            op = &s_ops[i];
            run_fn = op->run_fn;
            ctx = op->ctx;
            break;
        }
    }
    if (op != NULL) {
        if (!app_op_transition_allowed(op->state, APP_OP_CONNECTING)) {
            op = NULL;
        } else {
            op->state = APP_OP_CONNECTING;
            op->error = APP_OPERATION_ERR_NONE;
        }
    }
    portEXIT_CRITICAL(&s_ops_mux);

    if (op == NULL || run_fn == NULL) {
        return;
    }

    ESP_LOGI(TAG, "run %s -> connecting", op->id);
    err = run_fn(op, ctx);
    if (err != ESP_OK) {
        bool failed = false;

        ESP_LOGW(TAG, "run %s failed: %s", op->id, esp_err_to_name(err));
        portENTER_CRITICAL(&s_ops_mux);
        if (op->used && app_op_state_is_active(op->state)) {
            op->state = APP_OP_FAILED;
            op->error = APP_OPERATION_ERR_INTERNAL;
            op->deadline_ms = 0;
            op->terminal_ms = app_operation_now_ms();
            failed = true;
        }
        portEXIT_CRITICAL(&s_ops_mux);
        if (failed) {
            app_mqtt_defer_restart_end();
        }
    }
}

static void app_operation_expire_one(void)
{
    app_operation_t *op = NULL;
    app_operation_cleanup_fn cleanup = NULL;
    void *ctx = NULL;
    uint8_t generation = 0;
    bool cleanup_ok = true;
    int64_t now_ms = app_operation_now_ms();

    portENTER_CRITICAL(&s_ops_mux);
    for (size_t i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        app_operation_t *candidate = &s_ops[i];

        if (!candidate->used || !app_op_state_is_active(candidate->state) ||
            candidate->expiring || candidate->deadline_ms == 0 ||
            now_ms < candidate->deadline_ms) {
            continue;
        }
        candidate->expiring = true;
        generation = candidate->generation;
        cleanup = candidate->cleanup_fn;
        ctx = candidate->ctx;
        op = candidate;
        break;
    }
    portEXIT_CRITICAL(&s_ops_mux);

    if (op == NULL) {
        return;
    }
    if (cleanup != NULL) {
        cleanup_ok = cleanup(op, ctx);
    }

    {
        bool expired = false;

        portENTER_CRITICAL(&s_ops_mux);
        if (op->used && op->generation == generation && app_op_state_is_active(op->state)) {
            op->state = cleanup_ok ? APP_OP_EXPIRED : APP_OP_FAILED;
            op->error = cleanup_ok ? APP_OPERATION_ERR_TIMEOUT
                                   : APP_OPERATION_ERR_CLEANUP_FAILED;
            op->deadline_ms = 0;
            op->terminal_ms = app_operation_now_ms();
            expired = true;
        }
        op->expiring = false;
        portEXIT_CRITICAL(&s_ops_mux);
        if (expired) {
            app_mqtt_defer_restart_end();
        }
    }
    ESP_LOGW(TAG, "expire %s cleanup=%s", op->id,
             cleanup_ok ? "ok" : "failed");
}

static void app_operation_worker(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));

        portENTER_CRITICAL(&s_ops_mux);
        app_operation_reclaim_terminal_locked(app_operation_now_ms());
        portEXIT_CRITICAL(&s_ops_mux);

        app_operation_expire_one();
        app_operation_run_pending();
    }
}

static esp_err_t app_operation_ensure_worker(void)
{
    portENTER_CRITICAL(&s_ops_mux);
    if (s_worker_started) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_ops_mux);

#if defined(APP_OPERATION_WORKER_STATIC) && APP_OPERATION_WORKER_STATIC
    s_worker = xTaskCreateStatic(app_operation_worker, "op_worker",
                                 APP_OPERATION_WORKER_STACK_DEPTH,
                                 NULL, 4, s_worker_stack, &s_worker_tcb);
    if (s_worker == NULL) {
        ESP_LOGE(TAG, "static worker create failed");
        return ESP_ERR_NO_MEM;
    }
#else
    {
        BaseType_t rt = xTaskCreate(app_operation_worker, "op_worker",
                                    APP_OPERATION_WORKER_STACK,
                                    NULL, 4, &s_worker);

        if (rt != pdPASS) {
            ESP_LOGE(TAG, "dynamic worker create failed");
            s_worker = NULL;
            return ESP_ERR_NO_MEM;
        }
    }
#endif
    portENTER_CRITICAL(&s_ops_mux);
    s_worker_started = true;
    portEXIT_CRITICAL(&s_ops_mux);
#if defined(APP_OPERATION_WORKER_STATIC) && APP_OPERATION_WORKER_STATIC
    ESP_LOGI(TAG, "worker started static (stack=%uB depth=%u elems StackType_t=%uB tcb=%uB)",
             (unsigned)sizeof(s_worker_stack),
             (unsigned)APP_OPERATION_WORKER_STACK_DEPTH,
             (unsigned)sizeof(StackType_t),
             (unsigned)sizeof(s_worker_tcb));
#else
    ESP_LOGI(TAG, "worker started dynamic (stack=%uB)",
             (unsigned)APP_OPERATION_WORKER_STACK);
#endif
    return ESP_OK;
}

#ifdef APP_OPERATION_SELFTEST

static esp_err_t app_operation_selftest_got_ip(app_operation_t *op, void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(200));
    return app_operation_set_state(op, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE);
}

static esp_err_t app_operation_selftest_success(app_operation_t *op, void *ctx)
{
    esp_err_t err;

    (void)ctx;
    err = app_operation_set_state(op, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE);
    if (err != ESP_OK) {
        return err;
    }
    err = app_operation_set_state(op, APP_OP_COMMITTING, APP_OPERATION_ERR_NONE);
    if (err != ESP_OK) {
        return err;
    }
    return app_operation_set_state(op, APP_OP_SUCCESS, APP_OPERATION_ERR_NONE);
}

static bool app_operation_selftest_wait_state(app_operation_t *op, app_op_state_t want,
                                              uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        if (app_operation_state(op) == want) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return app_operation_state(op) == want;
}

void app_operation_set_test_retention_ms(uint32_t ms)
{
    s_retention_ms = ms;
}

static void app_operation_selftest_run(void)
{
    app_operation_t *op = NULL;
    app_operation_t *fill[3] = {0};
    size_t pass = 0;
    size_t fail = 0;
    esp_err_t err;

    ESP_LOGI(TAG, "selftest start heap=%u largest=%u",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    app_operation_set_test_retention_ms(1500);
    err = app_operation_start(APP_OPERATION_OWNER_INTERNAL, "self-gotip",
                              5000, 1000, app_operation_selftest_got_ip,
                              NULL, NULL, &op);
    if (err == ESP_OK) pass++; else fail++;
    ESP_LOGI(TAG, "selftest create got_ip: %s state=%s",
             esp_err_to_name(err), app_op_state_name(app_operation_state(op)));

    {
        app_operation_t *busy = NULL;
        err = app_operation_start(APP_OPERATION_OWNER_INTERNAL, "self-busy",
                                  5000, 1000, app_operation_selftest_got_ip,
                                  NULL, NULL, &busy);
        if (err == ESP_ERR_INVALID_STATE) pass++; else fail++;
        ESP_LOGI(TAG, "selftest concurrent create: %s", esp_err_to_name(err));
    }

    if (app_operation_selftest_wait_state(op, APP_OP_GOT_IP, 1500)) pass++; else fail++;
    ESP_LOGI(TAG, "selftest got_ip state=%s heap=%u largest=%u",
             app_op_state_name(app_operation_state(op)),
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));

    err = app_operation_cancel(op);
    if (err == ESP_OK && app_operation_state(op) == APP_OP_CANCELLED) pass++; else fail++;
    ESP_LOGI(TAG, "selftest cancel: %s state=%s", esp_err_to_name(err),
             app_op_state_name(app_operation_state(op)));
    {
        char cancelled_id[APP_OPERATION_ID_LEN];

        snprintf(cancelled_id, sizeof(cancelled_id), "%s", app_operation_id(op));

        for (size_t i = 0; i < sizeof(fill) / sizeof(fill[0]); i++) {
            err = app_operation_start(APP_OPERATION_OWNER_INTERNAL, "self-fill",
                                      5000, 1000, app_operation_selftest_success,
                                      NULL, NULL, &fill[i]);
            if (err == ESP_OK &&
                app_operation_selftest_wait_state(fill[i], APP_OP_SUCCESS, 1500)) {
                pass++;
            } else {
                fail++;
            }
            ESP_LOGI(TAG, "selftest fill[%u]: %s state=%s", (unsigned)i,
                     esp_err_to_name(err),
                     app_op_state_name(app_operation_state(fill[i])));
        }

        /* 槽满不再返回 NO_MEM：覆盖最老终态（本例即 cancelled 那条），
         * 新请求必须成功受理，且旧 id 查不到。 */
        {
            app_operation_t *overflow = NULL;
            err = app_operation_start(APP_OPERATION_OWNER_INTERNAL, "self-overflow",
                                      5000, 1000, app_operation_selftest_success,
                                      NULL, NULL, &overflow);
            if (err == ESP_OK && app_operation_find(cancelled_id) == NULL) {
                pass++;
            } else {
                fail++;
            }
            ESP_LOGI(TAG, "selftest slot overflow: %s old_id_found=%d",
                     esp_err_to_name(err), (int)(app_operation_find(cancelled_id) != NULL));
            (void)app_operation_selftest_wait_state(overflow, APP_OP_SUCCESS, 1500);
        }
    }

    /* 等终态保留过期，再验证 got_ip TTL。 */
    vTaskDelay(pdMS_TO_TICKS(1800));
    op = NULL;
    err = app_operation_start(APP_OPERATION_OWNER_INTERNAL, "self-ttl",
                              5000, 300, app_operation_selftest_got_ip,
                              NULL, NULL, &op);
    if (err == ESP_OK && app_operation_selftest_wait_state(op, APP_OP_GOT_IP, 1000)) {
        pass++;
    } else {
        fail++;
    }
    ESP_LOGI(TAG, "selftest ttl got_ip: %s state=%s",
             esp_err_to_name(err), app_op_state_name(app_operation_state(op)));
    if (app_operation_selftest_wait_state(op, APP_OP_EXPIRED, 2000)) pass++; else fail++;
    ESP_LOGI(TAG, "selftest ttl expired: state=%s", app_op_state_name(app_operation_state(op)));

    ESP_LOGI(TAG, "selftest done pass=%u fail=%u heap=%u largest=%u",
             (unsigned)pass, (unsigned)fail,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
}

void app_operation_selftest_start(void)
{
    /* 测试构建在 app_main 尾部同步执行：不额外创建 selftest 任务，
     * 这样堆预算只反映 operation worker 的常驻成本。 */
    app_operation_selftest_run();
}

#endif /* APP_OPERATION_SELFTEST */

#ifdef APP_OPERATION_LIFECYCLE_TEST

static esp_err_t app_operation_lifecycle_run(app_operation_t *op, void *ctx)
{
    (void)ctx;
    vTaskDelay(pdMS_TO_TICKS(300));
    return app_operation_set_state(op, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE);
}

static bool app_operation_lifecycle_wait(app_operation_t *op, app_op_state_t want,
                                         uint32_t timeout_ms)
{
    TickType_t deadline = xTaskGetTickCount() + pdMS_TO_TICKS(timeout_ms);

    while ((int32_t)(xTaskGetTickCount() - deadline) < 0) {
        if (app_operation_state(op) == want) {
            return true;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return app_operation_state(op) == want;
}

static void app_operation_lifecycle_log(const char *phase, size_t min_free_seen,
                                        size_t min_largest_seen)
{
    size_t free_now = esp_get_free_heap_size();
    size_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    size_t min_now = esp_get_minimum_free_heap_size();
    bool mqtt_connected;
    bool ble_connected = app_ble_gatts_connected();

    app_mqtt_lock();
    mqtt_connected = s_mqtt.connected;
    app_mqtt_unlock();

    ESP_LOGI(TAG,
             "lifecycle %s free=%u largest=%u min=%u phase_min_free=%u phase_min_largest=%u "
             "mqtt=%d ble=%d shed=%u gate=%s",
             phase,
             (unsigned)free_now, (unsigned)largest_now, (unsigned)min_now,
             (unsigned)(min_free_seen != 0 ? min_free_seen : free_now),
             (unsigned)(min_largest_seen != 0 ? min_largest_seen : largest_now),
             (int)mqtt_connected, (int)ble_connected,
             (unsigned)app_mqtt_tele_shed_count(),
             ((min_free_seen != 0 ? min_free_seen : free_now) >= 8192 &&
              (min_largest_seen != 0 ? min_largest_seen : largest_now) >= 6144)
                 ? "PASS" : "FAIL");
}

static void app_operation_lifecycle_update_min(size_t *min_free, size_t *min_largest)
{
    size_t free_now = esp_get_free_heap_size();
    size_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

    if (free_now < *min_free) {
        *min_free = free_now;
    }
    if (largest_now < *min_largest) {
        *min_largest = largest_now;
    }
}

static void app_operation_lifecycle_test_run(void)
{
    app_operation_t *op = NULL;
    esp_err_t err;
    bool stable = false;
    size_t min_free_seen;
    size_t min_largest_seen;
    size_t hw_bytes = 0;

    /* 等 MQTT 已连接且尚有最低可运行空间；静态 worker 的 4KB 在 .bss，
     * MQTT 连接后 baseline 可能已低于门槛，但仍必须测出并如实判 FAIL。 */
    for (int i = 0; i < 120; i++) {
        size_t free_now = esp_get_free_heap_size();
        size_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
        bool mqtt_connected;

        app_mqtt_lock();
        mqtt_connected = s_mqtt.connected;
        app_mqtt_unlock();
        if (mqtt_connected && free_now >= 4096 && largest_now >= 2048) {
            stable = true;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(500));
    }

    /* 先创建一次 defer timer，使 A/B 两组的 baseline 都包含同一项常驻开销，
     * 后续 worker 增量只反映任务栈/TCB 的创建方式差异。 */
    if (app_mqtt_defer_restart_begin() == ESP_OK) {
        app_mqtt_defer_restart_end();
    }

    min_free_seen = esp_get_free_heap_size();
    min_largest_seen = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    app_operation_lifecycle_log("baseline", min_free_seen, min_largest_seen);
    if (!stable) {
        ESP_LOGW(TAG, "lifecycle SKIP: stable MQTT window not reached");
        return;
    }

    err = app_operation_start(APP_OPERATION_OWNER_BLE, "lifecycle",
                              5000, 10000, app_operation_lifecycle_run,
                              NULL, NULL, &op);
    app_operation_lifecycle_update_min(&min_free_seen, &min_largest_seen);
    app_operation_lifecycle_log("after_start", min_free_seen, min_largest_seen);
    ESP_LOGI(TAG, "lifecycle defer after_start: deferred=%d pending=%d",
             (int)app_mqtt_restart_is_deferred(),
             (int)app_mqtt_restart_is_pending());
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lifecycle worker create/start failed: %s", esp_err_to_name(err));
        return;
    }

    if (!app_operation_lifecycle_wait(op, APP_OP_GOT_IP, 2000)) {
        ESP_LOGW(TAG, "lifecycle did not reach got_ip");
    }
    app_operation_lifecycle_update_min(&min_free_seen, &min_largest_seen);
    app_operation_lifecycle_log("got_ip", min_free_seen, min_largest_seen);

    /* operation 活动期间请求 MQTT restart：应被 defer/合并，不产生重建峰值。 */
    (void)app_mqtt_schedule_restart();
    ESP_LOGI(TAG, "lifecycle defer after_schedule: deferred=%d pending=%d",
             (int)app_mqtt_restart_is_deferred(),
             (int)app_mqtt_restart_is_pending());
    for (int i = 0; i < 20; i++) {
        size_t free_now = esp_get_free_heap_size();
        size_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

        if (free_now < min_free_seen) {
            min_free_seen = free_now;
        }
        if (largest_now < min_largest_seen) {
            min_largest_seen = largest_now;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    app_operation_lifecycle_log("mqtt_overlap", min_free_seen, min_largest_seen);
    ESP_LOGI(TAG, "lifecycle defer after_overlap: deferred=%d pending=%d",
             (int)app_mqtt_restart_is_deferred(),
             (int)app_mqtt_restart_is_pending());

    err = app_operation_cancel(op);
    app_operation_lifecycle_update_min(&min_free_seen, &min_largest_seen);
    app_operation_lifecycle_log("cancelled", min_free_seen, min_largest_seen);
    ESP_LOGI(TAG, "lifecycle cancel: %s state=%s defer=%d pending=%d",
             esp_err_to_name(err), app_op_state_name(app_operation_state(op)),
             (int)app_mqtt_restart_is_deferred(),
             (int)app_mqtt_restart_is_pending());

    /* 终态释放 defer 后，延迟 restart 仍可能形成峰值；完整采样 20 秒，
     * 覆盖 MQTT 退避重连和再次分配的峰值。 */
    for (int i = 0; i < 80; i++) {
        size_t free_now = esp_get_free_heap_size();
        size_t largest_now = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

        if (free_now < min_free_seen) {
            min_free_seen = free_now;
        }
        if (largest_now < min_largest_seen) {
            min_largest_seen = largest_now;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
    app_operation_lifecycle_log("post_restart", min_free_seen, min_largest_seen);

    if (s_worker != NULL) {
        hw_bytes = (size_t)uxTaskGetStackHighWaterMark(s_worker) * sizeof(StackType_t);
        ESP_LOGI(TAG, "lifecycle worker stack high-water=%uB (free=%uB)",
                 (unsigned)hw_bytes, (unsigned)(APP_OPERATION_WORKER_STACK - hw_bytes));
    }
    ESP_LOGI(TAG, "lifecycle done; worker remains resident (deletion not implemented)");
}

static void app_operation_lifecycle_timer(void *arg)
{
    (void)arg;
    app_operation_lifecycle_test_run();
}

void app_operation_lifecycle_test_start(void)
{
    static esp_timer_handle_t timer;
    const esp_timer_create_args_t args = {
        .callback = app_operation_lifecycle_timer,
        .name = "op_lifecycle",
    };
    esp_err_t err;

    if (timer != NULL) {
        return;
    }
    err = esp_timer_create(&args, &timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lifecycle timer create failed: %s", esp_err_to_name(err));
        return;
    }
    /* 让 app_main 返回、MQTT 先进入稳态，再在已有 esp_timer 任务里测量。 */
    err = esp_timer_start_once(timer, 30ULL * 1000000ULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "lifecycle timer start failed: %s", esp_err_to_name(err));
    }
}

#endif /* APP_OPERATION_LIFECYCLE_TEST */
