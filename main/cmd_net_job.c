#include "cmd_net_job.h"

#include <stdio.h>
#include <string.h>

#include "bt_gatts.h"
#include "common.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "netcheck.h"
#include "wifi.h"

static const char *TAG = "cmd";

#define APP_NET_TRIAL_TTL_MS (300 * 1000)
/* cleanup 认领等待上限：候选初值，待实测后定稿。 */
#define APP_CMD_NET_CLEANUP_WAIT_MS 200

typedef struct {
    bool valid;
    app_cmd_source_t owner;
    app_cmd_cfg_txn_t cfg_txn;
    bool cfg_txn_valid;
    char id[APP_CMD_NET_TRIAL_ID_LEN];
    char operation_id[APP_CMD_NET_TRIAL_OPERATION_ID_LEN];
    char ssid[APP_CMD_NET_TRIAL_SSID_LEN];
    char password[APP_CMD_NET_TRIAL_PASSWORD_LEN];
    int64_t expires_ms;
} app_cmd_net_trial_t;

typedef struct {
    bool active;
    app_cmd_source_t owner;
    app_cmd_context_t parent_ctx;
    char idem_key[65];
    char ssid[APP_CMD_NET_TRIAL_SSID_LEN];
    char password[APP_CMD_NET_TRIAL_PASSWORD_LEN];
    char operation_id[APP_CMD_NET_TRIAL_OPERATION_ID_LEN];
    app_cmd_cfg_txn_t cfg_txn;
    bool cfg_txn_valid;
    app_operation_t *op;
    uint32_t netcheck_gen;
    bool st_wifi_connected;
    bool st_ip_acquired;
    bool st_internet_verified;
    bool st_mqtt_connected;
} app_cmd_net_job_t;

static app_cmd_net_trial_t s_trial;
static app_cmd_net_job_t s_job;
static bool s_apply_internal;
static app_cmd_net_job_port_t s_port;
static bool s_inited;

#ifdef APP_CMD_NET_JOB_TEST
static app_cmd_net_job_test_hook_fn s_test_hook;

void app_cmd_net_job_test_hook_set(app_cmd_net_job_test_hook_fn fn)
{
    s_test_hook = fn;
}

static void net_test_hook(const char *point, app_operation_t *op, uint32_t expected_gen)
{
    if (s_test_hook != NULL) {
        s_test_hook(point, op, expected_gen);
    }
}
#else
static void net_test_hook(const char *point, app_operation_t *op, uint32_t expected_gen)
{
    (void)point;
    (void)op;
    (void)expected_gen;
}
#endif

#ifdef APP_DEV_TEST_HELPER
static app_cmd_net_job_dev_test_hook_fn s_dev_test_hook;
static bool s_dev_test_hook_set;
static uint32_t s_dev_claim_count;
static uint32_t s_dev_restore_request_count;
static bool s_dev_last_restore_ok;
static int64_t s_dev_wait_start_us;
static int64_t s_dev_wait_end_us;
static uint32_t s_dev_wait_limit_ticks;
static uint32_t s_dev_wait_waited_ticks;
static app_operation_cleanup_state_t s_dev_wait_claim_at_return =
    APP_OPERATION_CLEANUP_NONE;

void app_cmd_net_job_dev_test_hook_set(app_cmd_net_job_dev_test_hook_fn fn)
{
    /* 单向初始化：首个设置生效，运行期间不再替换/清空。 */
    if (!s_dev_test_hook_set) {
        s_dev_test_hook = fn;
        s_dev_test_hook_set = true;
    }
}

void app_cmd_net_job_dev_test_obs_reset(void)
{
    s_dev_claim_count = 0;
    s_dev_restore_request_count = 0;
    s_dev_last_restore_ok = false;
    s_dev_wait_start_us = 0;
    s_dev_wait_end_us = 0;
    s_dev_wait_limit_ticks = 0;
    s_dev_wait_waited_ticks = 0;
    s_dev_wait_claim_at_return = APP_OPERATION_CLEANUP_NONE;
}

uint32_t app_cmd_net_job_dev_test_claim_count(void)
{
    return s_dev_claim_count;
}

uint32_t app_cmd_net_job_dev_test_restore_request_count(void)
{
    return s_dev_restore_request_count;
}

bool app_cmd_net_job_dev_test_last_restore_ok(void)
{
    return s_dev_last_restore_ok;
}

int64_t app_cmd_net_job_dev_test_wait_start_us(void)
{
    return s_dev_wait_start_us;
}

int64_t app_cmd_net_job_dev_test_wait_end_us(void)
{
    return s_dev_wait_end_us;
}

uint32_t app_cmd_net_job_dev_test_wait_limit_ticks(void)
{
    return s_dev_wait_limit_ticks;
}

uint32_t app_cmd_net_job_dev_test_wait_waited_ticks(void)
{
    return s_dev_wait_waited_ticks;
}

app_operation_cleanup_state_t app_cmd_net_job_dev_test_wait_claim_at_return(void)
{
    return s_dev_wait_claim_at_return;
}
#endif

static bool app_cmd_net_job_owned_by_locked(const app_operation_t *op, uint32_t expected_gen);
static void app_cmd_net_job_clear_impl_locked(void);
static app_operation_cleanup_outcome_t app_cmd_net_finish_run(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation);

static void app_cmd_net_wipe(char *buf, size_t len)
{
    volatile char *p = (volatile char *)(void *)buf;

    while (len--) {
        *p++ = 0;
    }
}

static void app_cmd_net_job_clear_impl_locked(void)
{
    if (s_job.cfg_txn_valid) {
        /* end() 失败只表示 token 已失效或不匹配(例如已被先前的清理释放)；
         * 单 token 门没有额外资源需要回收，句柄可安全丢弃。 */
        (void)app_cmd_cfg_txn_end(s_job.cfg_txn);
    }
    app_cmd_net_wipe(s_job.password, sizeof(s_job.password));
    memset(&s_job, 0, sizeof(s_job));
}

void app_cmd_net_job_clear_locked(void)
{
    app_cmd_net_job_clear_impl_locked();
}

void app_cmd_net_trial_clear_locked(void)
{
    if (s_trial.cfg_txn_valid) {
        /* 同 job clear：end() 失败表示 token 已失效/不匹配，可安全丢弃。 */
        (void)app_cmd_cfg_txn_end(s_trial.cfg_txn);
    }
    app_cmd_net_wipe(s_trial.password, sizeof(s_trial.password));
    memset(&s_trial, 0, sizeof(s_trial));
}

void app_cmd_net_job_lock(void)
{
    if (s_port.lock != NULL) {
        s_port.lock();
    }
}

void app_cmd_net_job_unlock(void)
{
    if (s_port.unlock != NULL) {
        s_port.unlock();
    }
}

static void app_cmd_net_clear_active_id(const char *op_id)
{
    if (s_port.clear_active_operation != NULL) {
        s_port.clear_active_operation(op_id);
    }
}

static bool app_cmd_net_job_owned_by_locked(const app_operation_t *op, uint32_t expected_gen)
{
    return s_inited && op != NULL && s_job.active && s_job.op == op &&
           app_operation_generation_matches(op, expected_gen);
}

bool app_cmd_net_job_is_active(void)
{
    bool active;

    app_cmd_net_job_lock();
    active = s_job.active;
    app_cmd_net_job_unlock();
    return active;
}

bool app_cmd_net_job_apply_internal(void)
{
    bool on;

    app_cmd_net_job_lock();
    on = s_apply_internal;
    app_cmd_net_job_unlock();
    return on;
}

void app_cmd_net_job_set_apply_internal_locked(bool on)
{
    s_apply_internal = on;
}

bool app_cmd_net_job_accept_snapshot(bool *active, app_operation_t **op,
                                     char *idem_key, size_t idem_key_size)
{
    if (!s_inited || active == NULL || op == NULL || idem_key == NULL ||
        idem_key_size == 0) {
        return false;
    }
    app_cmd_net_job_lock();
    *active = s_job.active;
    *op = s_job.op;
    snprintf(idem_key, idem_key_size, "%s", s_job.idem_key);
    app_cmd_net_job_unlock();
    return true;
}

app_cmd_net_job_begin_result_t app_cmd_net_job_begin_locked(
    app_cmd_source_t source, app_operation_owner_t owner,
    const app_cmd_context_t *ctx, const char *ssid, const char *password,
    const char *idem_key, uint32_t accepted_ttl, uint32_t got_ip_ttl,
    app_operation_run_fn run_fn, app_operation_t **out_op)
{
    app_cmd_cfg_txn_t txn;
    app_operation_t *op = NULL;
    esp_err_t err;

    if (out_op != NULL) {
        *out_op = NULL;
    }
    if (!s_inited) {
        return APP_CMD_NET_JOB_BEGIN_NOT_READY;
    }
    if (s_job.active) {
        return APP_CMD_NET_JOB_BEGIN_BUSY;
    }
    /* 与旧实现一致：受理前 job 必须为空；clear 只处理理论上残留的事务。 */
    app_cmd_net_job_clear_impl_locked();
    if (!app_cmd_cfg_txn_begin(&txn)) {
        return APP_CMD_NET_JOB_BEGIN_NO_TXN;
    }
    s_job.cfg_txn = txn;
    s_job.cfg_txn_valid = true;
    s_job.active = true;
    s_job.owner = source;
    s_job.parent_ctx = *ctx;
    snprintf(s_job.ssid, sizeof(s_job.ssid), "%s", ssid);
    if (password != NULL) {
        snprintf(s_job.password, sizeof(s_job.password), "%s", password);
    }
    if (idem_key != NULL) {
        snprintf(s_job.idem_key, sizeof(s_job.idem_key), "%s", idem_key);
    }

    err = app_operation_start_with_finish(owner, idem_key, accepted_ttl, got_ip_ttl,
                                          run_fn, app_cmd_net_cleanup_run,
                                          app_cmd_net_finish_run, &s_job, &op);
    if (err != ESP_OK) {
        app_cmd_net_job_clear_impl_locked();
        return (err == ESP_ERR_INVALID_STATE) ? APP_CMD_NET_JOB_BEGIN_BUSY
                                             : APP_CMD_NET_JOB_BEGIN_NO_SLOT;
    }
    s_job.op = op;
    snprintf(s_job.operation_id, sizeof(s_job.operation_id), "%s",
             app_operation_id(op));
    if (out_op != NULL) {
        *out_op = op;
    }
    return APP_CMD_NET_JOB_BEGIN_OK;
}

bool app_cmd_net_job_clear_if_owned(const app_operation_t *op, uint32_t expected_gen)
{
    bool owned;

    net_test_hook("clear_before_lock", (app_operation_t *)op, expected_gen);

    app_cmd_net_job_lock();
    owned = app_cmd_net_job_owned_by_locked(op, expected_gen);
    if (owned) {
        app_cmd_net_job_clear_impl_locked();
    }
    app_cmd_net_job_unlock();
    if (!owned) {
        ESP_LOGW(TAG, "job clear skipped: operation no longer owns job");
    }
    return owned;
}

bool app_cmd_net_job_worker_snapshot(const app_operation_t *op, uint32_t expected_gen,
                                     char *ssid, size_t ssid_size,
                                     char *password, size_t password_size,
                                     app_cmd_source_t *owner)
{
    bool owned = false;
    int n1 = 0;
    int n2 = 0;

    if (ssid == NULL || password == NULL || owner == NULL ||
        ssid_size == 0 || password_size == 0) {
        return false;
    }
    app_cmd_net_job_lock();
    if (app_cmd_net_job_owned_by_locked(op, expected_gen)) {
        n1 = snprintf(ssid, ssid_size, "%s", s_job.ssid);
        n2 = snprintf(password, password_size, "%s", s_job.password);
        if (n1 >= 0 && (size_t)n1 < ssid_size &&
            n2 >= 0 && (size_t)n2 < password_size) {
            *owner = s_job.owner;
            owned = true;
        } else {
            ssid[0] = '\0';
            /* 截断时可能已写入密码前缀:擦除整个输出缓冲区。 */
            app_cmd_net_wipe(password, password_size);
        }
    }
    app_cmd_net_job_unlock();
    return owned;
}

bool app_cmd_net_job_set_netcheck_gen(const app_operation_t *op, uint32_t expected_gen,
                                      uint32_t link_gen)
{
    bool owned;

    net_test_hook("netcheck_before_lock", (app_operation_t *)op, expected_gen);

    app_cmd_net_job_lock();
    owned = app_cmd_net_job_owned_by_locked(op, expected_gen);
    if (owned) {
        s_job.netcheck_gen = link_gen;
    }
    app_cmd_net_job_unlock();
    return owned;
}

bool app_cmd_net_job_set_stages(const app_operation_t *op, uint32_t expected_gen,
                                bool wifi, bool ip, bool inet, bool mqtt)
{
    bool owned;

    net_test_hook("stages_before_lock", (app_operation_t *)op, expected_gen);

    app_cmd_net_job_lock();
    owned = app_cmd_net_job_owned_by_locked(op, expected_gen);
    if (owned) {
        s_job.st_wifi_connected = wifi;
        s_job.st_ip_acquired = ip;
        s_job.st_internet_verified = inet;
        s_job.st_mqtt_connected = mqtt;
    }
    app_cmd_net_job_unlock();
    return owned;
}

bool app_cmd_net_job_disconnect_target(app_operation_t **out_op)
{
    bool found = false;

    if (out_op != NULL) {
        *out_op = NULL;
    }
    app_cmd_net_job_lock();
    if (s_job.active && s_job.op != NULL && s_job.owner == APP_CMD_SOURCE_BLE) {
        if (out_op != NULL) {
            *out_op = s_job.op;
        }
        found = true;
    }
    app_cmd_net_job_unlock();
    return found;
}

void app_cmd_net_job_snapshot_locked(app_cmd_net_job_snap_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->active = s_job.active;
    snprintf(out->operation_id, sizeof(out->operation_id), "%s", s_job.operation_id);
    out->st_wifi_connected = s_job.st_wifi_connected;
    out->st_ip_acquired = s_job.st_ip_acquired;
    out->st_internet_verified = s_job.st_internet_verified;
    out->st_mqtt_connected = s_job.st_mqtt_connected;
    out->netcheck_gen = s_job.netcheck_gen;
}

bool app_cmd_net_job_parent_ctx_locked(app_cmd_context_t *out)
{
    if (out == NULL || !s_job.active) {
        return false;
    }
    *out = s_job.parent_ctx;
    return true;
}

app_cmd_net_trial_store_result_t app_cmd_net_trial_store(
    const char *ssid, const char *password, app_cmd_source_t source,
    const char *operation_id, app_cmd_cfg_txn_t *cfg_txn,
    const app_operation_t *op, bool *txn_taken)
{
    app_cmd_net_trial_store_result_t result;

    if (txn_taken != NULL) {
        *txn_taken = false;
    }
    if (!s_inited) {
        return APP_CMD_NET_TRIAL_REJECTED;
    }
    if (ssid == NULL || password == NULL) {
        return APP_CMD_NET_TRIAL_REJECTED;
    }
    net_test_hook("trial_before_lock", (app_operation_t *)op, 0);
    app_cmd_net_job_lock();
    if (op != NULL && (!s_job.active || s_job.op != op)) {
        result = s_job.active ? APP_CMD_NET_TRIAL_STALE_OP
                              : APP_CMD_NET_TRIAL_STALE_JOB;
        app_cmd_net_job_unlock();
        return result;
    }
    if (source == APP_CMD_SOURCE_BLE &&
        (s_port.ble_connected == NULL || !s_port.ble_connected())) {
        app_cmd_net_job_unlock();
        return APP_CMD_NET_TRIAL_REJECTED;
    }
    snprintf(s_trial.id, sizeof(s_trial.id), "trial-%08X", (unsigned)esp_random());
    snprintf(s_trial.ssid, sizeof(s_trial.ssid), "%s", ssid);
    snprintf(s_trial.password, sizeof(s_trial.password), "%s", password);
    if (operation_id != NULL) {
        snprintf(s_trial.operation_id, sizeof(s_trial.operation_id), "%s",
                 operation_id);
    }
    if (cfg_txn != NULL) {
        s_trial.cfg_txn = *cfg_txn;
        s_trial.cfg_txn_valid = true;
        if (txn_taken != NULL) {
            *txn_taken = true;
        }
    }
    s_trial.expires_ms = (s_port.now_ms != NULL ? s_port.now_ms() : 0) +
                         APP_NET_TRIAL_TTL_MS;
    s_trial.owner = source;
    s_trial.valid = true;
    app_cmd_net_job_unlock();
    return APP_CMD_NET_TRIAL_STORED;
}

bool app_cmd_net_trial_ready_locked(const char *trial_id)
{
    int64_t now;
    bool ok;

    if (!s_inited) {
        return false;
    }
    now = (s_port.now_ms != NULL ? s_port.now_ms() : 0);
    if (!s_trial.valid || now >= s_trial.expires_ms) {
        if (s_trial.valid) {
            app_cmd_net_trial_clear_locked();
        }
        ok = false;
    } else {
        ok = (trial_id == NULL || strcmp(trial_id, s_trial.id) == 0);
    }
    return ok;
}

bool app_cmd_net_trial_ready(const char *trial_id)
{
    bool ok;

    app_cmd_net_job_lock();
    ok = app_cmd_net_trial_ready_locked(trial_id);
    app_cmd_net_job_unlock();
    return ok;
}

void app_cmd_net_trial_clear(void)
{
    app_cmd_net_job_lock();
    app_cmd_net_trial_clear_locked();
    app_cmd_net_job_unlock();
}

bool app_cmd_net_trial_busy_locked(void)
{
    return s_trial.valid && app_cmd_net_trial_ready_locked(NULL);
}

bool app_cmd_net_trial_busy(void)
{
    bool busy;

    app_cmd_net_job_lock();
    busy = app_cmd_net_trial_busy_locked();
    app_cmd_net_job_unlock();
    return busy;
}

bool app_cmd_net_trial_disconnect_clear(void)
{
    bool cleared = false;

    app_cmd_net_job_lock();
    if (s_trial.valid && s_trial.owner == APP_CMD_SOURCE_BLE) {
        app_cmd_net_trial_clear_locked();
        cleared = true;
    }
    app_cmd_net_job_unlock();
    return cleared;
}

void app_cmd_net_trial_snapshot_locked(app_cmd_net_trial_snap_t *out)
{
    if (out == NULL) {
        return;
    }
    memset(out, 0, sizeof(*out));
    out->valid = s_trial.valid;
    if (s_trial.valid) {
        snprintf(out->id, sizeof(out->id), "%s", s_trial.id);
        snprintf(out->operation_id, sizeof(out->operation_id), "%s",
                 s_trial.operation_id);
        snprintf(out->ssid, sizeof(out->ssid), "%s", s_trial.ssid);
    }
}

bool app_cmd_net_trial_ready_for_operation_locked(const char *operation_id)
{
    return s_trial.valid && operation_id != NULL &&
           strcmp(s_trial.operation_id, operation_id) == 0 &&
           app_cmd_net_trial_ready_locked(s_trial.id);
}

bool app_cmd_net_trial_credentials_locked(char *ssid, size_t ssid_size,
                                          char *password, size_t password_size)
{
    if (ssid == NULL || password == NULL || ssid_size == 0 || password_size == 0) {
        return false;
    }
    if (!s_trial.valid) {
        ssid[0] = '\0';
        password[0] = '\0';
        return false;
    }
    snprintf(ssid, ssid_size, "%s", s_trial.ssid);
    snprintf(password, password_size, "%s", s_trial.password);
    return true;
}

/* P1 兼容边界：超时返回 false 会被调用方映射为 CLEANUP_DONE_FAIL。
 * 此时遗留 claimant 可能仍在执行，因此这不是最终生命周期语义；
 * P2 迁移全部 cleanup caller 后再移除固定等待，避免把“尚未完成”永久
 * 说成“已完成”或直接强制终态。 */
static bool app_cmd_net_cleanup_wait_result(const app_operation_t *op, uint32_t expected_gen,
                                            const char *op_id)
{
    app_operation_cleanup_state_t state = APP_OPERATION_CLEANUP_RUNNING;
    TickType_t limit = pdMS_TO_TICKS(APP_CMD_NET_CLEANUP_WAIT_MS);
    TickType_t waited;

    if (limit == 0) {
        limit = 1;
    }
#ifdef APP_DEV_TEST_HELPER
    s_dev_wait_start_us = esp_timer_get_time();
    s_dev_wait_limit_ticks = (uint32_t)limit;
    s_dev_wait_waited_ticks = 0;
    s_dev_wait_claim_at_return = APP_OPERATION_CLEANUP_NONE;
#endif
    for (waited = 0; waited < limit; waited++) {
        vTaskDelay(1);
        state = app_operation_cleanup_result(op, expected_gen);
        if (state != APP_OPERATION_CLEANUP_RUNNING) {
            break;
        }
    }
#ifdef APP_DEV_TEST_HELPER
    s_dev_wait_end_us = esp_timer_get_time();
    s_dev_wait_waited_ticks = (uint32_t)waited;
    s_dev_wait_claim_at_return = state;
#endif
    if (state == APP_OPERATION_CLEANUP_OK) {
        return true;
    }
    if (state == APP_OPERATION_CLEANUP_FAIL) {
        return false;
    }
    ESP_LOGW(TAG, "cleanup wait timeout op=%s", op_id);
    return false;
}

/* REQUIRED 的共享实现。job/trial 任一归本 op 都必须先 claim；
 * RUNNING 只等待，不触碰资源。旧 bool 入口和 P1 finish REQUIRED 共用。 */
static app_operation_cleanup_outcome_t app_cmd_net_cleanup_run_common(
    app_operation_t *op, uint32_t expected_gen)
{
    char op_id[APP_OPERATION_ID_LEN];
    app_operation_status_t st;
    bool job_owned;
    bool trial_match;
    bool restore_persistent;
    uint32_t nc_gen = 0;
    bool ok = true;
    app_operation_cleanup_claim_t claim;
    app_operation_cleanup_state_t existing = APP_OPERATION_CLEANUP_NONE;

    if (op == NULL) {
        return APP_OPERATION_CLEANUP_OUTCOME_STALE;
    }
    snprintf(op_id, sizeof(op_id), "%s", app_operation_id(op));

    app_cmd_net_job_lock();
    if (!app_operation_get_status(op, &st) || st.generation != expected_gen) {
        app_cmd_net_job_unlock();
        return APP_OPERATION_CLEANUP_OUTCOME_STALE;
    }
    job_owned = s_job.active && s_job.op == op &&
                app_operation_generation_matches(op, expected_gen);
    if (s_job.active && !job_owned) {
        app_cmd_net_job_unlock();
        return APP_OPERATION_CLEANUP_OUTCOME_STALE;
    }
    if (s_trial.valid && strcmp(s_trial.operation_id, op_id) != 0) {
        app_cmd_net_job_unlock();
        return APP_OPERATION_CLEANUP_OUTCOME_STALE;
    }
    trial_match = s_trial.valid;
    /* REQUIRED：trial-only 也可能代表未提交候选，不能仅因 job 已消失就
     * 跳过持久配置恢复。 */
    restore_persistent = st.state != APP_OP_COMMITTING;

    existing = app_operation_cleanup_result(op, expected_gen);
    if (existing == APP_OPERATION_CLEANUP_RUNNING) {
        app_cmd_net_job_unlock();
        ok = app_cmd_net_cleanup_wait_result(op, expected_gen, op_id);
        return ok ? APP_OPERATION_CLEANUP_OUTCOME_DONE_OK
                  : APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
    }
    if (!job_owned && !trial_match) {
        app_cmd_net_job_unlock();
        app_cmd_net_clear_active_id(op_id);
        if (existing == APP_OPERATION_CLEANUP_FAIL) {
            return APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
        }
        if (existing == APP_OPERATION_CLEANUP_OK) {
            return APP_OPERATION_CLEANUP_OUTCOME_DONE_OK;
        }
        ESP_LOGI(TAG, "CLEANUP_NO_JOB op=%s", op_id);
        return APP_OPERATION_CLEANUP_OUTCOME_NO_JOB;
    }
    if (existing == APP_OPERATION_CLEANUP_FAIL) {
        app_cmd_net_job_unlock();
        app_cmd_net_clear_active_id(op_id);
        return APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
    }
    if (existing == APP_OPERATION_CLEANUP_OK) {
        /* claim 已完成但仍有本 op 资源：所有权不一致，不重复释放。 */
        app_cmd_net_job_unlock();
        ESP_LOGE(TAG, "cleanup inconsistent: claim OK but resources remain op=%s", op_id);
        return APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
    }

    claim = app_operation_cleanup_claim(op, expected_gen);
#ifdef APP_DEV_TEST_HELPER
    if (claim == APP_OPERATION_CLEANUP_CLAIMED) {
        s_dev_claim_count++;
    }
#endif
    if (claim != APP_OPERATION_CLEANUP_CLAIMED) {
        app_cmd_net_job_unlock();
        ESP_LOGW(TAG, "cleanup claim anomaly op=%s claim=%d", op_id, (int)claim);
        if (claim == APP_OPERATION_CLEANUP_ALREADY_RUNNING) {
            ok = app_cmd_net_cleanup_wait_result(op, expected_gen, op_id);
            return ok ? APP_OPERATION_CLEANUP_OUTCOME_DONE_OK
                      : APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
        }
        if (claim == APP_OPERATION_CLEANUP_ALREADY_DONE_OK) {
            return APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
        }
        if (claim == APP_OPERATION_CLEANUP_STALE ||
            claim == APP_OPERATION_CLEANUP_INVALID) {
            return APP_OPERATION_CLEANUP_OUTCOME_STALE;
        }
        return APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
    }

    nc_gen = job_owned ? s_job.netcheck_gen : 0;
    if (trial_match) {
        app_cmd_net_trial_clear_locked();
    }
    if (job_owned) {
        app_cmd_net_job_clear_impl_locked();
    }
    app_cmd_net_job_unlock();
    app_cmd_net_clear_active_id(op_id);

    net_test_hook("cleanup_after_claim", op, expected_gen);
#ifdef APP_DEV_TEST_HELPER
    if (s_dev_test_hook != NULL) {
        s_dev_test_hook("cleanup_after_claim", op, expected_gen);
    }
#endif

    if (job_owned) {
        if (s_port.netcheck_cancel != NULL) {
            s_port.netcheck_cancel(nc_gen);
        }
        if (st.state == APP_OP_CONNECTING && s_port.try_cancel != NULL) {
            (void)s_port.try_cancel();
        }
    }
    if (restore_persistent) {
        ESP_LOGW(TAG, "rescue=restore_persistent op=%s (cleanup without commit)", op_id);
        ok = false;
        if (s_port.cleanup_request != NULL) {
#ifdef APP_DEV_TEST_HELPER
            s_dev_restore_request_count++;
            s_dev_last_restore_ok = false;
#endif
            ok = s_port.cleanup_request(op_id);
#ifdef APP_DEV_TEST_HELPER
            s_dev_last_restore_ok = ok;
#endif
        }
        if (!ok) {
            ESP_LOGE(TAG, "rescue=restore_persistent failed (cleanup request) op=%s", op_id);
        }
    }

    net_test_hook("cleanup_before_complete", op, expected_gen);
    app_operation_cleanup_complete(op, expected_gen, ok);
    if (ok) {
        ESP_LOGI(TAG, "CLEANUP_DONE_OK op=%s", op_id);
    } else {
        ESP_LOGW(TAG, "CLEANUP_DONE_FAIL op=%s", op_id);
    }
    return ok ? APP_OPERATION_CLEANUP_OUTCOME_DONE_OK
              : APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
}

bool app_cmd_net_cleanup_run(app_operation_t *op, void *ctx)
{
    app_operation_status_t st;
    app_operation_cleanup_outcome_t outcome;

    (void)ctx;
    if (op == NULL || !app_operation_get_status(op, &st)) {
        return false;
    }
    outcome = app_cmd_net_cleanup_run_common(op, st.generation);
    return outcome == APP_OPERATION_CLEANUP_OUTCOME_DONE_OK ||
           outcome == APP_OPERATION_CLEANUP_OUTCOME_NO_JOB;
}

app_cmd_net_release_result_t app_cmd_net_release_if_owned(
    app_operation_t *op, uint32_t expected_gen)
{
    char op_id[APP_CMD_NET_TRIAL_OPERATION_ID_LEN];
    app_operation_status_t st;
    bool job_owned;
    bool trial_match;
    app_operation_cleanup_state_t existing;
    app_operation_cleanup_claim_t claim;

    if (op == NULL) {
        return APP_CMD_NET_RELEASE_STALE;
    }
    snprintf(op_id, sizeof(op_id), "%s", app_operation_id(op));

    app_cmd_net_job_lock();
    if (!app_operation_get_status(op, &st) || st.generation != expected_gen) {
        app_cmd_net_job_unlock();
        return APP_CMD_NET_RELEASE_STALE;
    }
    if (s_job.active && s_job.op != op) {
        app_cmd_net_job_unlock();
        return APP_CMD_NET_RELEASE_STALE;
    }
    if (s_job.active && !app_operation_generation_matches(op, expected_gen)) {
        app_cmd_net_job_unlock();
        return APP_CMD_NET_RELEASE_STALE;
    }
    job_owned = s_job.active && s_job.op == op;
    if (s_trial.valid) {
        if (strcmp(s_trial.operation_id, op_id) != 0) {
            app_cmd_net_job_unlock();
            return APP_CMD_NET_RELEASE_STALE;
        }
        trial_match = true;
    } else {
        trial_match = false;
    }

    existing = app_operation_cleanup_result(op, expected_gen);
    if (!job_owned && !trial_match) {
        app_cmd_net_job_unlock();
        if (existing == APP_OPERATION_CLEANUP_RUNNING) {
            return APP_CMD_NET_RELEASE_PENDING;
        }
        if (existing == APP_OPERATION_CLEANUP_FAIL) {
            return APP_CMD_NET_RELEASE_FAIL;
        }
        if (s_port.clear_active_operation != NULL) {
            s_port.clear_active_operation(op_id);
        }
        return APP_CMD_NET_RELEASE_NO_RESOURCE;
    }

    claim = app_operation_cleanup_claim(op, expected_gen);
    switch (claim) {
    case APP_OPERATION_CLEANUP_CLAIMED:
        if (job_owned) {
            app_cmd_net_job_clear_impl_locked();
        }
        if (trial_match) {
            app_cmd_net_trial_clear_locked();
        }
        app_cmd_net_job_unlock();
        app_operation_cleanup_complete(op, expected_gen, true);
        if (s_port.clear_active_operation != NULL) {
            s_port.clear_active_operation(op_id);
        }
        return APP_CMD_NET_RELEASE_DONE;
    case APP_OPERATION_CLEANUP_ALREADY_RUNNING:
        app_cmd_net_job_unlock();
        return APP_CMD_NET_RELEASE_PENDING;
    case APP_OPERATION_CLEANUP_ALREADY_DONE_OK:
        app_cmd_net_job_unlock();
        if (job_owned || trial_match) {
            ESP_LOGE(TAG, "release inconsistent: claim OK but resources remain op=%s",
                     op_id);
            return APP_CMD_NET_RELEASE_FAIL;
        }
        if (s_port.clear_active_operation != NULL) {
            s_port.clear_active_operation(op_id);
        }
        return APP_CMD_NET_RELEASE_NO_RESOURCE;
    case APP_OPERATION_CLEANUP_ALREADY_DONE_FAIL:
        app_cmd_net_job_unlock();
        return APP_CMD_NET_RELEASE_FAIL;
    case APP_OPERATION_CLEANUP_INVALID:
    case APP_OPERATION_CLEANUP_STALE:
    default:
        app_cmd_net_job_unlock();
        return APP_CMD_NET_RELEASE_STALE;
    }
}

static app_operation_cleanup_outcome_t app_cmd_net_finish_run(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation)
{
    (void)ctx;
    if (op == NULL) {
        return APP_OPERATION_CLEANUP_OUTCOME_STALE;
    }
    switch (policy) {
    case APP_OPERATION_CLEANUP_REQUIRED:
        return app_cmd_net_cleanup_run_common(op, expected_generation);
    case APP_OPERATION_CLEANUP_RELEASE_ONLY:
        switch (app_cmd_net_release_if_owned(op, expected_generation)) {
        case APP_CMD_NET_RELEASE_DONE:
            return APP_OPERATION_CLEANUP_OUTCOME_DONE_OK;
        case APP_CMD_NET_RELEASE_NO_RESOURCE:
            return APP_OPERATION_CLEANUP_OUTCOME_NO_JOB;
        case APP_CMD_NET_RELEASE_PENDING:
            return APP_OPERATION_CLEANUP_OUTCOME_PENDING;
        case APP_CMD_NET_RELEASE_STALE:
            return APP_OPERATION_CLEANUP_OUTCOME_STALE;
        case APP_CMD_NET_RELEASE_FAIL:
        default:
            return APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL;
        }
    case APP_OPERATION_CLEANUP_NO_RESOURCE_RELEASE:
    default:
        return APP_OPERATION_CLEANUP_OUTCOME_NO_JOB;
    }
}

#ifndef APP_CMD_NET_JOB_TEST
static SemaphoreHandle_t s_prod_lock = NULL;
static StaticSemaphore_t s_prod_lock_buf;
static portMUX_TYPE s_prod_lock_mux = portMUX_INITIALIZER_UNLOCKED;

static void app_cmd_net_prod_lock(void)
{
    xSemaphoreTakeRecursive(app_mutex_ensure(&s_prod_lock, &s_prod_lock_buf,
                                             &s_prod_lock_mux, true), portMAX_DELAY);
}

static void app_cmd_net_prod_unlock(void)
{
    if (s_prod_lock != NULL) {
        xSemaphoreGiveRecursive(s_prod_lock);
    }
}

static bool app_cmd_net_prod_ble_connected(void)
{
    return app_ble_gatts_connected();
}

static bool app_cmd_net_prod_cleanup_request(const char *op_id)
{
    return app_wifi_cleanup_request(op_id);
}

static bool app_cmd_net_prod_try_cancel(void)
{
    return app_wifi_try_cancel();
}

static void app_cmd_net_prod_clear_active_operation(const char *op_id)
{
    app_wifi_clear_active_operation(op_id);
}

static void app_cmd_net_prod_netcheck_cancel(uint32_t link_gen)
{
    app_netcheck_cancel(link_gen);
}

static int64_t app_cmd_net_prod_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const app_cmd_net_job_port_t s_default_port = {
    .lock = app_cmd_net_prod_lock,
    .unlock = app_cmd_net_prod_unlock,
    .ble_connected = app_cmd_net_prod_ble_connected,
    .cleanup_request = app_cmd_net_prod_cleanup_request,
    .try_cancel = app_cmd_net_prod_try_cancel,
    .clear_active_operation = app_cmd_net_prod_clear_active_operation,
    .netcheck_cancel = app_cmd_net_prod_netcheck_cancel,
    .now_ms = app_cmd_net_prod_now_ms,
};

esp_err_t app_cmd_net_job_init(void)
{
    if (s_inited) {
        return ESP_ERR_INVALID_STATE;
    }
    s_port = s_default_port;
    s_inited = true;
    return ESP_OK;
}
#else
esp_err_t app_cmd_net_job_init_with_port(const app_cmd_net_job_port_t *port)
{
    if (s_inited || port == NULL || port->lock == NULL || port->unlock == NULL ||
        port->ble_connected == NULL || port->cleanup_request == NULL ||
        port->try_cancel == NULL || port->clear_active_operation == NULL ||
        port->netcheck_cancel == NULL || port->now_ms == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_port = *port;
    s_inited = true;
    return ESP_OK;
}
#endif
