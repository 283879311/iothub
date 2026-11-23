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

enum {
    APP_OPERATION_FINISH_PHASE_NONE = 0,
    APP_OPERATION_FINISH_PHASE_REGISTERED,
    APP_OPERATION_FINISH_PHASE_CALLBACK_RUNNING,
    APP_OPERATION_FINISH_PHASE_CALLBACK_PENDING_EXTERNAL,
    APP_OPERATION_FINISH_PHASE_DONE,
};

struct app_operation {
    bool used;
    bool expiring;
    bool running;      /* run_fn 在飞:期间禁止本槽回收/复用(R5) */
    uint8_t net_cleanup_state;   /* CMD-03b:cleanup 一次性认领状态 */
    uint32_t net_cleanup_gen;    /* 认领时的 generation 快照(诊断用) */
    uint32_t generation;
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
    app_operation_finish_fn finish_fn;
    void *ctx;
    /* P1 finish intent：仅登记一次；第二个请求只返回 PENDING。 */
    bool finish_registered;
    uint8_t finish_phase;
    uint32_t finish_generation;
    app_op_state_t finish_target_state;
    app_operation_error_t finish_target_error;
    app_operation_cleanup_policy_t finish_policy;
    int64_t finish_started_ms;
    uint16_t finish_pending_normal_count;
    uint16_t finish_pending_external_count;
};

static app_operation_t s_ops[APP_OPERATION_MAX_SLOTS];
static portMUX_TYPE s_ops_mux = portMUX_INITIALIZER_UNLOCKED;
static TaskHandle_t s_worker = NULL;
static bool s_worker_started = false;
static bool s_expire_started = false;
static bool s_initialized = false;
static uint32_t s_retention_ms = APP_OPERATION_DEFAULT_RETENTION_MS;

/* ---------------------------------------------------------------------------
 * C1 复审确定性钩子(仅 APP_C1_TEST_HOOKS 实验构建):
 * 武装后,下一次 cancel/expire 在 cleanup 返回、finalize 之前暂停 hold_ms;
 * 暂停入口对同一 op 直接调一次 set_state(probe_to_state),把结果打到串口
 * —— "expiring=true 时一切迁移被拒"由此获得确定性证据,不依赖随机调度。
 * 单次武装;探针在 ops_mux 临界区外调用(此刻两把锁都空闲),安全。
 * ------------------------------------------------------------------------- */
#ifdef APP_C1_TEST_HOOKS
static int64_t app_operation_now_ms(void);
static bool s_c1_hook_armed;
static int64_t s_c1_hook_until_ms;
static bool s_c1_hook_probe;
static app_op_state_t s_c1_hook_probe_to;

void app_operation_test_hook_arm(uint32_t hold_ms, int probe_to_state)
{
    s_c1_hook_until_ms = app_operation_now_ms() + (int64_t)hold_ms;
    s_c1_hook_probe = (probe_to_state > (int)APP_OP_FREE &&
                       probe_to_state < (int)APP_OP_STATE_COUNT);
    s_c1_hook_probe_to = (app_op_state_t)probe_to_state;
    s_c1_hook_armed = true;
    ESP_LOGW(TAG, "C1 hook armed: hold=%ums probe=%s", (unsigned)hold_ms,
             s_c1_hook_probe ? app_op_state_name(s_c1_hook_probe_to) : "none");
}

void app_operation_test_hook_clear(void)
{
    s_c1_hook_armed = false;
    s_c1_hook_probe = false;
    ESP_LOGW(TAG, "C1 hook cleared");
}

static void app_operation_test_hook_pause(const char *tag, app_operation_t *op)
{
    int64_t until;

    if (!s_c1_hook_armed) {
        return;
    }
    s_c1_hook_armed = false;
    until = s_c1_hook_until_ms;
    ESP_LOGW(TAG, "C1 hook: pause %s enter (expiring=%d state=%s gen=%u)",
             tag, (int)op->expiring, app_op_state_name(op->state),
             (unsigned)op->generation);
    if (s_c1_hook_probe) {
        esp_err_t probe_err = app_operation_set_state(op, s_c1_hook_probe_to,
                                                      APP_OPERATION_ERR_NONE);
        ESP_LOGW(TAG, "C1 hook: probe set_state(%s) -> %s (0x%x)%s",
                 app_op_state_name(s_c1_hook_probe_to),
                 esp_err_to_name(probe_err), (unsigned)probe_err,
                 probe_err == ESP_ERR_INVALID_STATE
                     ? " [rejected by expiring gate]"
                     : "");
        s_c1_hook_probe = false;
    }
    while (app_operation_now_ms() < until) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    ESP_LOGW(TAG, "C1 hook: pause %s exit", tag);
}
#endif

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

/* C 语言不允许对不同数组对象的指针做 < / >= 关系比较;这里用逐槽位相等
 * 判断,避免以未定义行为判断"指针是否属于 s_ops"。 */
static bool app_operation_in_table(const app_operation_t *op)
{
    size_t i;

    if (op == NULL) {
        return false;
    }
    for (i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        if (op == &s_ops[i]) {
            return true;
        }
    }
    return false;
}

static bool app_operation_valid(const app_operation_t *op)
{
    return app_operation_in_table(op) && op->used;
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
    case APP_OPERATION_ERR_WIFI_CONFIG_OFFLINE: return "wifi_config_offline";
    case APP_OPERATION_ERR_MQTT_UNAVAILABLE: return "mqtt_unavailable";
    case APP_OPERATION_ERR_INTERNET_UNVERIFIED: return "internet_unverified";
    case APP_OPERATION_ERR_STATIC_IP_PINNED: return "static_ip_pinned";
    case APP_OPERATION_ERR_LAST_PROFILE: return "last_profile";
    case APP_OPERATION_ERR_VERIFY_TIMEOUT: return "verify_timeout";
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

        if (!op->used || op->running || !app_op_state_is_terminal(op->state)) {
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

        if (!op->used || op->running || !app_op_state_is_terminal(op->state)) {
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

esp_err_t app_operation_start_with_finish(app_operation_owner_t owner, const char *idem_key,
                                          uint32_t accepted_ttl_ms, uint32_t got_ip_ttl_ms,
                                          app_operation_run_fn run_fn,
                                          app_operation_cleanup_fn cleanup_fn,
                                          app_operation_finish_fn finish_fn,
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
            app_mqtt_defer_restart_end_at("op_create/busy");
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
                app_mqtt_defer_restart_end_at("op_create/no_slot");
            }
            return ESP_ERR_NO_MEM;      /* 理论不可达：无活动且无终态却无空槽 */
        }
        /* 临界区内只取证，日志在锁外打 */
        snprintf(reclaimed_id, sizeof(reclaimed_id), "%s", slot->id);
        reclaimed_state = slot->state;
        reclaimed_age_ms = now_ms - slot->terminal_ms;
    }

    /* 评审 R4:generation 必须在复用前+1并跨 memset 保留 —— 原实现先清零再
     * ++,每次复用都回到 1,陈旧 cancel/expire 捕获的 generation 会与新 op
     * 撞上(槽满时 oldest_terminal 立即回收,窗口不是 600s)。32 位回绕仅在
     * 40 亿次复用后发生,期间槽位早已加速回收,不再作为实际风险。 */
    {
        uint32_t next_generation = slot->generation + 1;

        memset(slot, 0, sizeof(*slot));
        slot->generation = next_generation;
    }
    slot->used = true;
    slot->owner = owner;
    slot->state = APP_OP_ACCEPTED;
    slot->error = APP_OPERATION_ERR_NONE;
    slot->created_ms = now_ms;
    slot->deadline_ms = now_ms + (int64_t)accepted_ttl_ms;
    slot->got_ip_ttl_ms = got_ip_ttl_ms;
    slot->run_fn = run_fn;
    slot->cleanup_fn = cleanup_fn;
    slot->finish_fn = finish_fn;
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
            app_mqtt_defer_restart_end_at("op_create/error");
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

esp_err_t app_operation_start(app_operation_owner_t owner, const char *idem_key,
                              uint32_t accepted_ttl_ms, uint32_t got_ip_ttl_ms,
                              app_operation_run_fn run_fn,
                              app_operation_cleanup_fn cleanup_fn,
                              void *ctx, app_operation_t **out)
{
    return app_operation_start_with_finish(owner, idem_key, accepted_ttl_ms, got_ip_ttl_ms,
                                           run_fn, cleanup_fn, NULL, ctx, out);
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
    /* 评审 R3-T1/T2:expiring=true 表示 cancel/expire 的 cleanup 已在飞、
     * 本 op 注定终态 —— 拒绝一切新状态迁移。由此 "cleanup 已启动" 与
     * "进入 COMMITTING" 由同一把 s_ops_mux 原子裁决,不可能同时成立:
     * 输掉竞争的 apply 在读 job 数据之前就被挡下(cleanup 期间状态仍是
     * GOT_IP,无本闸时 set_state 会成功、随后与 memset 撕裂 job)。 */
    if (op->expiring) {
        portEXIT_CRITICAL(&s_ops_mux);
        return ESP_ERR_INVALID_STATE;
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
        app_mqtt_defer_restart_end_at("op_set_state/terminal");
    }
    return ESP_OK;
}

esp_err_t app_operation_cancel(app_operation_t *op)
{
    app_operation_cleanup_fn cleanup = NULL;
    void *ctx = NULL;
    uint32_t generation;
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
            app_mqtt_defer_restart_end_at("op_cancel/cleanup_failed");
        }
        return ESP_FAIL;
    }

#ifdef APP_C1_TEST_HOOKS
    app_operation_test_hook_pause("cancel_post_cleanup", op);
#endif
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
        app_mqtt_defer_restart_end_at("op_cancel/terminal");
    }
    return ESP_OK;
}

/* 仅在 s_ops_mux 内调用；成功后 operation 已终态化。 */
static bool app_operation_finish_apply_locked(app_operation_t *op,
                                               app_operation_cleanup_outcome_t outcome)
{
    app_op_state_t final_state;
    app_operation_error_t final_error;

    if (!op->used || !op->finish_registered ||
        op->finish_generation != op->generation ||
        !app_op_state_is_active(op->state)) {
        return false;
    }
    if (outcome == APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL) {
        final_state = APP_OP_FAILED;
        final_error = APP_OPERATION_ERR_CLEANUP_FAILED;
    } else {
        final_state = op->finish_target_state;
        final_error = op->finish_target_error;
    }
    if (!app_op_transition_allowed(op->state, final_state)) {
        return false;
    }
    op->state = final_state;
    op->error = final_error;
    op->deadline_ms = 0;
    op->terminal_ms = app_operation_now_ms();
    op->finish_phase = APP_OPERATION_FINISH_PHASE_DONE;
    op->expiring = false;
    return true;
}

app_operation_finish_result_t
app_operation_request_finish(app_operation_t *op, uint32_t expected_generation,
                             app_op_state_t target_state,
                             app_operation_error_t target_error,
                             app_operation_cleanup_policy_t cleanup_policy)
{
    app_operation_cleanup_fn cleanup_fn;
    app_operation_finish_fn finish_fn;
    void *ctx;
    bool run_callback = false;
    bool legacy_cleanup = false;
    app_operation_cleanup_outcome_t outcome;
    char op_id[APP_OPERATION_ID_LEN] = "";
    uint32_t log_gen = 0;

    if (!app_operation_in_table(op)) {
        return APP_OPERATION_FINISH_STALE;
    }

    portENTER_CRITICAL(&s_ops_mux);
    if (!op->used || op->generation != expected_generation) {
        portEXIT_CRITICAL(&s_ops_mux);
        return APP_OPERATION_FINISH_STALE;
    }
    if (op->state == APP_OP_COMMITTING) {
        portEXIT_CRITICAL(&s_ops_mux);
        return APP_OPERATION_FINISH_REJECTED;
    }
    if (app_op_state_is_terminal(op->state)) {
        portEXIT_CRITICAL(&s_ops_mux);
        return APP_OPERATION_FINISH_REJECTED;
    }
    if (op->finish_registered) {
        if (op->finish_pending_normal_count != UINT16_MAX) {
            op->finish_pending_normal_count++;
        }
        portEXIT_CRITICAL(&s_ops_mux);
        return APP_OPERATION_FINISH_PENDING;
    }
    if (target_state == APP_OP_FREE || !app_op_state_is_terminal(target_state) ||
        !app_op_transition_allowed(op->state, target_state)) {
        portEXIT_CRITICAL(&s_ops_mux);
        return APP_OPERATION_FINISH_REJECTED;
    }
    if (cleanup_policy == APP_OPERATION_CLEANUP_RELEASE_ONLY &&
        op->finish_fn == NULL) {
        portEXIT_CRITICAL(&s_ops_mux);
        return APP_OPERATION_FINISH_REJECTED;
    }

    cleanup_fn = op->cleanup_fn;
    finish_fn = op->finish_fn;
    ctx = op->ctx;
    if (cleanup_policy != APP_OPERATION_CLEANUP_NO_RESOURCE_RELEASE) {
        if (cleanup_policy == APP_OPERATION_CLEANUP_RELEASE_ONLY) {
            run_callback = (finish_fn != NULL);
        } else if (finish_fn != NULL) {
            run_callback = true;
            legacy_cleanup = false;
        } else if (cleanup_fn != NULL) {
            run_callback = true;
            legacy_cleanup = true;
        }
    }

    op->finish_registered = true;
    op->finish_phase = APP_OPERATION_FINISH_PHASE_REGISTERED;
    op->finish_generation = expected_generation;
    op->finish_target_state = target_state;
    op->finish_target_error = target_error;
    op->finish_policy = cleanup_policy;
    op->finish_started_ms = app_operation_now_ms();
    op->expiring = true;
    snprintf(op_id, sizeof(op_id), "%s", op->id);
    log_gen = op->generation;
    if (run_callback) {
        op->finish_phase = APP_OPERATION_FINISH_PHASE_CALLBACK_RUNNING;
    }
    portEXIT_CRITICAL(&s_ops_mux);

    if (!run_callback) {
        bool applied;

        portENTER_CRITICAL(&s_ops_mux);
        applied = app_operation_finish_apply_locked(
            op, APP_OPERATION_CLEANUP_OUTCOME_DONE_OK);
        portEXIT_CRITICAL(&s_ops_mux);
        if (!applied) {
            return APP_OPERATION_FINISH_STALE;
        }
        app_mqtt_defer_restart_end_at("op_finish/no_resource");
        ESP_LOGI(TAG, "finish %s gen=%u -> %s (no callback)",
                 op_id, (unsigned)log_gen, app_op_state_name(target_state));
        return APP_OPERATION_FINISH_DONE;
    }

    if (legacy_cleanup) {
        outcome = cleanup_fn(op, ctx)
                      ? APP_OPERATION_CLEANUP_OUTCOME_DONE_OK
                      : APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
    } else {
        outcome = finish_fn(op, ctx, cleanup_policy);
    }

#ifdef APP_C1_TEST_HOOKS
    /* 保留 C1 实验构建的 expire 暂停点：cleanup 已返回、终态发布之前。
     * 仅测试构建存在，不改变生产路径。 */
    if (outcome != APP_OPERATION_CLEANUP_OUTCOME_PENDING &&
        outcome != APP_OPERATION_CLEANUP_OUTCOME_STALE &&
        op->finish_target_state == APP_OP_EXPIRED) {
        app_operation_test_hook_pause("expire_post_cleanup", op);
    }
#endif

    portENTER_CRITICAL(&s_ops_mux);
    if (!op->used || op->generation != expected_generation ||
        op->finish_generation != expected_generation ||
        !op->finish_registered) {
        portEXIT_CRITICAL(&s_ops_mux);
        ESP_LOGW(TAG, "finish %s gen=%u stale after callback", op_id,
                 (unsigned)log_gen);
        return APP_OPERATION_FINISH_STALE;
    }
    if (outcome == APP_OPERATION_CLEANUP_OUTCOME_PENDING) {
        op->finish_phase = APP_OPERATION_FINISH_PHASE_CALLBACK_PENDING_EXTERNAL;
        if (op->finish_pending_external_count != UINT16_MAX) {
            op->finish_pending_external_count++;
        }
        portEXIT_CRITICAL(&s_ops_mux);
        ESP_LOGE(TAG, "finish %s gen=%u external cleanup pending (claim running)",
                 op_id, (unsigned)log_gen);
        return APP_OPERATION_FINISH_PENDING;
    }
    if (outcome == APP_OPERATION_CLEANUP_OUTCOME_STALE) {
        portEXIT_CRITICAL(&s_ops_mux);
        ESP_LOGW(TAG, "finish %s gen=%u cleanup stale; operation left active",
                 op_id, (unsigned)log_gen);
        return APP_OPERATION_FINISH_STALE;
    }
    if (!app_operation_finish_apply_locked(op, outcome)) {
        portEXIT_CRITICAL(&s_ops_mux);
        ESP_LOGW(TAG, "finish %s gen=%u could not apply final state", op_id,
                 (unsigned)log_gen);
        return APP_OPERATION_FINISH_STALE;
    }
    {
        app_op_state_t final_state = op->state;
        app_operation_error_t final_error = op->error;

        portEXIT_CRITICAL(&s_ops_mux);
        app_mqtt_defer_restart_end_at("op_finish/terminal");
        ESP_LOGI(TAG, "finish %s gen=%u -> %s/%s outcome=%d",
                 op_id, (unsigned)log_gen, app_op_state_name(final_state),
                 app_operation_error_name(final_error), (int)outcome);
    }
    return APP_OPERATION_FINISH_DONE;
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
        out->cleanup_state = (app_operation_cleanup_state_t)op->net_cleanup_state;
        ok = true;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return ok;
}

bool app_operation_generation_matches(const app_operation_t *op, uint32_t expected_generation)
{
    bool match = false;

    if (!app_operation_in_table(op)) {
        return false;
    }
    portENTER_CRITICAL(&s_ops_mux);
    match = op->used && op->state != APP_OP_FREE &&
            op->generation == expected_generation;
    portEXIT_CRITICAL(&s_ops_mux);
    return match;
}

app_operation_cleanup_claim_t app_operation_cleanup_claim(app_operation_t *op,
                                                          uint32_t expected_generation)
{
    app_operation_cleanup_claim_t result;

    if (!app_operation_in_table(op)) {
        return APP_OPERATION_CLEANUP_INVALID;
    }
    portENTER_CRITICAL(&s_ops_mux);
    if (!op->used || op->state == APP_OP_FREE) {
        result = APP_OPERATION_CLEANUP_INVALID;
    } else if (op->generation != expected_generation) {
        result = APP_OPERATION_CLEANUP_STALE;
    } else if (!app_op_state_is_active(op->state)) {
        /* 生命周期保证 cleanup 只在该 op 活动期间被调用(框架 cancel/expire
         * 在终态化之前调用,worker 直调在 CONNECTING)。若仍收到终态槽位,
         * 视为陈旧/非法调用:不认领、不执行副作用。已完成结果改由
         * app_operation_cleanup_result() 读取(非认领者路径)。 */
        result = APP_OPERATION_CLEANUP_INVALID;
    } else if (op->net_cleanup_state == APP_OPERATION_CLEANUP_RUNNING) {
        result = APP_OPERATION_CLEANUP_ALREADY_RUNNING;
    } else if (op->net_cleanup_state == APP_OPERATION_CLEANUP_OK) {
        result = APP_OPERATION_CLEANUP_ALREADY_DONE_OK;
    } else if (op->net_cleanup_state == APP_OPERATION_CLEANUP_FAIL) {
        result = APP_OPERATION_CLEANUP_ALREADY_DONE_FAIL;
    } else {
        op->net_cleanup_state = APP_OPERATION_CLEANUP_RUNNING;
        op->net_cleanup_gen = expected_generation;
        result = APP_OPERATION_CLEANUP_CLAIMED;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return result;
}

void app_operation_cleanup_complete(app_operation_t *op, uint32_t expected_generation, bool ok)
{
    bool ignored = false;

    if (!app_operation_in_table(op)) {
        return;
    }
    portENTER_CRITICAL(&s_ops_mux);
    if (op->used && op->generation == expected_generation &&
        op->net_cleanup_gen == expected_generation &&
        op->net_cleanup_state == APP_OPERATION_CLEANUP_RUNNING) {
        op->net_cleanup_state = ok ? APP_OPERATION_CLEANUP_OK
                                   : APP_OPERATION_CLEANUP_FAIL;
    } else {
        ignored = true;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    if (ignored) {
        ESP_LOGW(TAG, "cleanup complete ignored: generation/claim mismatch");
    }
}

app_operation_cleanup_state_t app_operation_cleanup_result(const app_operation_t *op,
                                                           uint32_t expected_generation)
{
    app_operation_cleanup_state_t state = APP_OPERATION_CLEANUP_NONE;

    if (!app_operation_in_table(op)) {
        return state;
    }
    portENTER_CRITICAL(&s_ops_mux);
    if (op->used && op->generation == expected_generation) {
        state = (app_operation_cleanup_state_t)op->net_cleanup_state;
    }
    portEXIT_CRITICAL(&s_ops_mux);
    return state;
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
            op->running = true;   /* R5:run_fn 返回前禁止本槽回收/复用 */
        }
    }
    portEXIT_CRITICAL(&s_ops_mux);

    if (op == NULL || run_fn == NULL) {
        return;
    }

    ESP_LOGI(TAG, "run %s -> connecting", op->id);
    err = run_fn(op, ctx);
    portENTER_CRITICAL(&s_ops_mux);
    op->running = false;   /* run_fn 已返回:此后才允许回收/复用本槽 */
    portEXIT_CRITICAL(&s_ops_mux);
    if (err != ESP_OK) {
        bool failed = false;
        bool skip_for_cleanup = false;

        ESP_LOGW(TAG, "run %s failed: %s", op->id, esp_err_to_name(err));
        /* 评审 R4:expiring=true 表示 cleanup 已在飞 —— run_fn 的 set_state
         * 已被 expiring 闸拒绝,这里不能再把终态改成 FAILED,否则会出现
         * "对外 FAILED、cleanup 仍在执行",且 cleanup 的 finalize 因状态
         * 已非 active 而落空。终态交由 cleanup 独占裁决(CANCELLED/
         * EXPIRED/FAILED(cleanup_failed))。
         * R7 勘误:跳过分支的日志曾在临界区内(ESP_LOG 禁入 spinlock,
         * 钩子首测即 abort);现在锁内只取判据,日志移到锁外。 */
        portENTER_CRITICAL(&s_ops_mux);
        if (op->used && op->expiring) {
            skip_for_cleanup = true;
        } else if (op->used && app_op_state_is_active(op->state)) {
            op->state = APP_OP_FAILED;
            op->error = APP_OPERATION_ERR_INTERNAL;
            op->deadline_ms = 0;
            op->terminal_ms = app_operation_now_ms();
            failed = true;
        }
        portEXIT_CRITICAL(&s_ops_mux);
        if (skip_for_cleanup) {
            ESP_LOGI(TAG, "run %s fallback skipped: cleanup owns terminal (expiring=1)",
                     op->id);
        }
        if (failed) {
            app_mqtt_defer_restart_end_at("worker/run_failed");
        }
    }
}

static void app_operation_expire_one(void)
{
    app_operation_t *op = NULL;
    uint32_t generation = 0;
    int64_t now_ms = app_operation_now_ms();
    char op_id[APP_OPERATION_ID_LEN] = "";
    app_operation_finish_result_t finish;

    portENTER_CRITICAL(&s_ops_mux);
    for (size_t i = 0; i < APP_OPERATION_MAX_SLOTS; i++) {
        app_operation_t *candidate = &s_ops[i];

        if (!candidate->used || !app_op_state_is_active(candidate->state) ||
            candidate->expiring || candidate->finish_registered ||
            candidate->deadline_ms == 0 || now_ms < candidate->deadline_ms) {
            continue;
        }
        op = candidate;
        generation = candidate->generation;
        snprintf(op_id, sizeof(op_id), "%s", candidate->id);
        break;
    }
    portEXIT_CRITICAL(&s_ops_mux);

    if (op == NULL) {
        return;
    }
    finish = app_operation_request_finish(op, generation, APP_OP_EXPIRED,
                                          APP_OPERATION_ERR_TIMEOUT,
                                          APP_OPERATION_CLEANUP_REQUIRED);
    if (finish == APP_OPERATION_FINISH_DONE) {
        ESP_LOGW(TAG, "expire %s finished", op_id);
    } else if (finish == APP_OPERATION_FINISH_PENDING) {
        ESP_LOGW(TAG, "expire %s pending (another finish owner)", op_id);
    } else if (finish == APP_OPERATION_FINISH_REJECTED) {
        ESP_LOGW(TAG, "expire %s rejected", op_id);
    } else {
        ESP_LOGW(TAG, "expire %s stale", op_id);
    }
}

/* DEF-MQTT-002(2026-11-05):独立过期任务。
 * 原实现里 expire_one() 与 run_fn() 在同一个 worker 任务内串行:run_fn
 * 阻塞(试验在飞)时 1s 节拍停摆,accepted TTL 永远不到期,孤儿占槽到
 * 重启。这里拆出小任务只跑 expire_one();并发安全由既有的 expiring 闸 +
 * generation 比对保证(R3/R4 评审前提)。
 * 不用 esp_timer:expire_one 同步调 cleanup_fn,cleanup 可能阻塞。 */
static void app_operation_expire_task(void *arg)
{
    (void)arg;

    for (;;) {
        (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(1000));
        app_operation_expire_one();
    }
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

    /* DEF-MQTT-002:过期任务与 run_fn 解耦(小栈,只做 expire_one) */
    if (!s_expire_started) {
        BaseType_t et = xTaskCreate(app_operation_expire_task, "op_expire", 3072,
                                    NULL, 4, NULL);

        if (et != pdPASS) {
            ESP_LOGE(TAG, "expire task create failed (TTL safety net disabled)");
        } else {
            s_expire_started = true;
            ESP_LOGI(TAG, "expire task started (stack=3072B, 1s tick, decoupled from run_fn)");
        }
    }
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
