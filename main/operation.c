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

#include "operation.h"

static const char *TAG = "operation";

#define APP_OPERATION_DEFAULT_ACCEPTED_TTL_MS 20000
#define APP_OPERATION_DEFAULT_GOT_IP_TTL_MS 300000
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

static int64_t app_operation_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static bool app_operation_valid(const app_operation_t *op)
{
    return op != NULL && op >= &s_ops[0] && op < &s_ops[APP_OPERATION_MAX_SLOTS] &&
           op->used;
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

    now_ms = app_operation_now_ms();
    portENTER_CRITICAL(&s_ops_mux);
    app_operation_reclaim_terminal_locked(now_ms);
    if (app_operation_has_active_locked()) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_STATE;   /* busy */
    }
    for (i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (!s_ops[i].used) {
            slot = &s_ops[i];
            break;
        }
    }
    if (slot == NULL) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_NO_MEM;          /* terminal slots still retained */
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

    if (app_operation_ensure_worker() != ESP_OK) {
        portENTER_CRITICAL(&s_ops_mux);
        slot->used = false;
        slot->state = APP_OP_FREE;
        portEXIT_CRITICAL(&s_ops_mux);
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

    if (!app_operation_valid(op)) {
        return ESP_ERR_INVALID_ARG;
    }
    now_ms = app_operation_now_ms();

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
    } else if (app_op_state_is_terminal(state)) {
        op->deadline_ms = 0;
        op->terminal_ms = now_ms;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return ESP_OK;
}

esp_err_t app_operation_cancel(app_operation_t *op)
{
    app_operation_cleanup_fn cleanup = NULL;
    void *ctx = NULL;
    uint8_t generation;
    bool need_cleanup = false;

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
        }
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_FAIL;
    }

    portENTER_CRITICAL(&s_ops_mux);
    if (op->used && op->generation == generation && app_op_state_is_active(op->state)) {
        op->state = APP_OP_CANCELLED;
        op->error = APP_OPERATION_ERR_CANCELLED;
        op->deadline_ms = 0;
        op->terminal_ms = app_operation_now_ms();
    }
    op->expiring = false;
    portEXIT_CRITICAL(&s_ops_mux);
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
        ESP_LOGW(TAG, "run %s failed: %s", op->id, esp_err_to_name(err));
        portENTER_CRITICAL(&s_ops_mux);
        if (op->used && app_op_state_is_active(op->state)) {
            op->state = APP_OP_FAILED;
            op->error = APP_OPERATION_ERR_INTERNAL;
            op->deadline_ms = 0;
            op->terminal_ms = app_operation_now_ms();
        }
        portEXIT_CRITICAL(&s_ops_mux);
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

    portENTER_CRITICAL(&s_ops_mux);
    if (op->used && op->generation == generation && app_op_state_is_active(op->state)) {
        op->state = cleanup_ok ? APP_OP_EXPIRED : APP_OP_FAILED;
        op->error = cleanup_ok ? APP_OPERATION_ERR_TIMEOUT
                               : APP_OPERATION_ERR_CLEANUP_FAILED;
        op->deadline_ms = 0;
        op->terminal_ms = app_operation_now_ms();
    }
    op->expiring = false;
    portEXIT_CRITICAL(&s_ops_mux);
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
    BaseType_t rt;

    portENTER_CRITICAL(&s_ops_mux);
    if (s_worker_started) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_OK;
    }
    portEXIT_CRITICAL(&s_ops_mux);

    rt = xTaskCreate(app_operation_worker, "op_worker", APP_OPERATION_WORKER_STACK,
                     NULL, 4, &s_worker);
    if (rt != pdPASS) {
        ESP_LOGE(TAG, "worker create failed");
        s_worker = NULL;
        return ESP_ERR_NO_MEM;
    }
    portENTER_CRITICAL(&s_ops_mux);
    s_worker_started = true;
    portEXIT_CRITICAL(&s_ops_mux);
    ESP_LOGI(TAG, "worker started (stack=%u)", (unsigned)APP_OPERATION_WORKER_STACK);
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

    {
        app_operation_t *overflow = NULL;
        err = app_operation_start(APP_OPERATION_OWNER_INTERNAL, "self-overflow",
                                  5000, 1000, app_operation_selftest_success,
                                  NULL, NULL, &overflow);
        if (err == ESP_ERR_NO_MEM) pass++; else fail++;
        ESP_LOGI(TAG, "selftest slot overflow: %s", esp_err_to_name(err));
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
