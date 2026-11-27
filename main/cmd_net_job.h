#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cmd_cfg_txn.h"
#include "cmd_context.h"
#include "operation.h"

/* CMD-03 H1：网络 job/trial 状态与 cleanup 归属协调。
 * 仅封装 job/trial 状态、J 锁、归属守卫与 cleanup claim；命令解析、apply 事务、
 * 状态响应构建和 BLE 断连事件处理仍留在 cmd.c。 */

#define APP_CMD_NET_TRIAL_ID_LEN 20
#define APP_CMD_NET_TRIAL_OPERATION_ID_LEN 20
#define APP_CMD_NET_TRIAL_SSID_LEN 33
#define APP_CMD_NET_TRIAL_PASSWORD_LEN 65

typedef enum {
    APP_CMD_NET_TRIAL_STORED = 0,
    APP_CMD_NET_TRIAL_REJECTED,      /* BLE 源已断连:丢弃候选(原语义) */
    APP_CMD_NET_TRIAL_STALE_JOB,     /* job 已清空或未激活(CMD-03a) */
    APP_CMD_NET_TRIAL_STALE_OP,      /* job 已归属其他 operation(CMD-03a) */
} app_cmd_net_trial_store_result_t;

typedef enum {
    APP_CMD_NET_JOB_BEGIN_OK = 0,
    APP_CMD_NET_JOB_BEGIN_BUSY,
    APP_CMD_NET_JOB_BEGIN_NO_SLOT,
    APP_CMD_NET_JOB_BEGIN_NO_TXN,
    APP_CMD_NET_JOB_BEGIN_NOT_READY,
} app_cmd_net_job_begin_result_t;

/* P1b：RELEASE_ONLY 的资源释放结果。 */
typedef enum {
    APP_CMD_NET_RELEASE_DONE = 0,
    APP_CMD_NET_RELEASE_NO_RESOURCE,
    APP_CMD_NET_RELEASE_PENDING,
    APP_CMD_NET_RELEASE_STALE,
    APP_CMD_NET_RELEASE_FAIL,
} app_cmd_net_release_result_t;

typedef struct {
    bool active;
    char operation_id[APP_CMD_NET_TRIAL_OPERATION_ID_LEN];
    bool st_wifi_connected;
    bool st_ip_acquired;
    bool st_internet_verified;
    bool st_mqtt_connected;
    uint32_t netcheck_gen;
} app_cmd_net_job_snap_t;

typedef struct {
    bool valid;
    char id[APP_CMD_NET_TRIAL_ID_LEN];
    char operation_id[APP_CMD_NET_TRIAL_OPERATION_ID_LEN];
    char ssid[APP_CMD_NET_TRIAL_SSID_LEN];
} app_cmd_net_trial_snap_t;

/* 生产初始化：模块内部绑定 FreeRTOS 锁、BLE、Wi-Fi、netcheck 与时钟。
 * 必须在任何 job/trial API 之前调用一次；重复调用返回 invalid state。 */
esp_err_t app_cmd_net_job_init(void);

/* 锁：保持现有递归语义；J → s_ops_mux 锁序。 */
void app_cmd_net_job_lock(void);
void app_cmd_net_job_unlock(void);

/* 状态读取（公开接口自取 J；*_locked 要求调用方已持 J）。 */
bool app_cmd_net_job_is_active(void);
bool app_cmd_net_job_apply_internal(void);
void app_cmd_net_job_set_apply_internal_locked(bool on);
bool app_cmd_net_job_accept_snapshot(bool *active, app_operation_t **op,
                                     char *idem_key, size_t idem_key_size);

/* 受理事务：内部完成 cfg_txn、job 初始化、operation_start 与 op 绑定；
 * 失败在同一 J 临界区内回滚。调用方必须已持 J。 */
app_cmd_net_job_begin_result_t app_cmd_net_job_begin_locked(
    app_cmd_source_t source, app_operation_owner_t owner,
    const app_cmd_context_t *ctx, const char *ssid, const char *password,
    const char *idem_key, uint32_t accepted_ttl, uint32_t got_ip_ttl,
    app_operation_run_fn run_fn, app_operation_t **out_op);

void app_cmd_net_job_clear_locked(void);
bool app_cmd_net_job_clear_if_owned(const app_operation_t *op, uint32_t expected_gen);

/* worker 运行时快照：不暴露内部结构体；失败表示 job 已不归本 op。 */
bool app_cmd_net_job_worker_snapshot(const app_operation_t *op, uint32_t expected_gen,
                                     char *ssid, size_t ssid_size,
                                     char *password, size_t password_size,
                                     app_cmd_source_t *owner);

/* worker 归属守卫写入；返回 false 表示已失去归属，调用方不得再触碰 job。 */
bool app_cmd_net_job_set_netcheck_gen(const app_operation_t *op, uint32_t expected_gen,
                                      uint32_t link_gen);
bool app_cmd_net_job_set_stages(const app_operation_t *op, uint32_t expected_gen,
                                bool wifi, bool ip, bool inet, bool mqtt);

/* BLE 断连：取当前 BLE 源 job 的 op（锁内快照）。 */
bool app_cmd_net_job_disconnect_target(app_operation_t **out_op);

/* 状态快照（调用方持 J）。 */
void app_cmd_net_job_snapshot_locked(app_cmd_net_job_snap_t *out);
bool app_cmd_net_job_parent_ctx_locked(app_cmd_context_t *out);

/* trial：store 自带归属检查（CMD-03a）；txn_taken=true 时调用方不得再 end。 */
app_cmd_net_trial_store_result_t app_cmd_net_trial_store(
    const char *ssid, const char *password, app_cmd_source_t source,
    const char *operation_id, app_cmd_cfg_txn_t *cfg_txn,
    const app_operation_t *op, bool *txn_taken);
bool app_cmd_net_trial_ready(const char *trial_id);
bool app_cmd_net_trial_ready_locked(const char *trial_id);
void app_cmd_net_trial_clear(void);
void app_cmd_net_trial_clear_locked(void);
bool app_cmd_net_trial_busy(void);
bool app_cmd_net_trial_busy_locked(void);
bool app_cmd_net_trial_disconnect_clear(void);
void app_cmd_net_trial_snapshot_locked(app_cmd_net_trial_snap_t *out);
bool app_cmd_net_trial_ready_for_operation_locked(const char *operation_id);
bool app_cmd_net_trial_credentials_locked(char *ssid, size_t ssid_size,
                                          char *password, size_t password_size);

/* operation cleanup 回调（注册进 operation_start）。 */
bool app_cmd_net_cleanup_run(app_operation_t *op, void *ctx);

/* P1b：RELEASE_ONLY。claim 与完整 cleanup 共用同一 authority；
 * 只释放本 op 拥有的 job/trial，不触发 netcheck/try/restore。 */
app_cmd_net_release_result_t app_cmd_net_release_if_owned(
    app_operation_t *op, uint32_t expected_gen);

/* 运行时副作用 port：Wi-Fi/BLE/netcheck/时钟/锁的唯一出口。
 * 生产实现由模块内部绑定；host 测试构建可显式传入。
 * 锁契约：ble_connected / now_ms 是无锁只读查询，允许在持 J 时调用；
 * 其余可能阻塞或产生副作用的回调必须在 J 锁外调用。 */
typedef struct app_cmd_net_job_port {
    void (*lock)(void);
    void (*unlock)(void);
    bool (*ble_connected)(void);
    bool (*cleanup_request)(const char *op_id);
    bool (*try_cancel)(void);
    void (*clear_active_operation)(const char *op_id);
    void (*netcheck_cancel)(uint32_t link_gen);
    int64_t (*now_ms)(void);
} app_cmd_net_job_port_t;

#ifdef APP_CMD_NET_JOB_TEST
/* host 测试入口：按值复制 port；仅测试构建可见。 */
esp_err_t app_cmd_net_job_init_with_port(const app_cmd_net_job_port_t *port);

/* 测试 hook：仅 APP_CMD_NET_JOB_TEST 构建存在；point 为冻结设计中的
 * trial_before_lock / netcheck_before_lock / stages_before_lock /
 * clear_before_lock / cleanup_after_claim / cleanup_before_complete。 */
typedef void (*app_cmd_net_job_test_hook_fn)(const char *point,
                                             app_operation_t *op,
                                             uint32_t expected_gen);
void app_cmd_net_job_test_hook_set(app_cmd_net_job_test_hook_fn fn);
#endif

#ifdef APP_C15_BUSY_DIAG
/* C15-2 BUSY 来源诊断（test-only）。capture_locked 要求调用方持 J；
 * capture 仅用于明确未持 J 的调用点。两入口都只读、不改变状态。 */
typedef struct {
    bool job_active_before;
    bool start_attempted;
    int operation_start_err;
    int begin_result;
    bool defer_attempted;
    bool defer_started;
    int defer_result;
    bool active_check_done;
    bool active_check_result;
} app_c15_begin_diag_t;

typedef struct {
    bool job_active;
    bool job_op_nonnull;
    bool job_op_valid;
    char job_operation_id[APP_OPERATION_ID_LEN];
    uint8_t job_owner;
    uint8_t job_op_state;
    bool job_op_running;
    uint32_t job_op_generation;
    bool idem_key_present;
    bool cfg_txn_valid;
    app_operation_diag_t ops;
    bool job_snapshot_locked;
    bool ops_snapshot_locked;
    bool cross_domain_atomic;   /* 固定 false：不宣称 J/O 跨域原子 */
    bool same_lock_as_decision;
    const char *snapshot_phase;
    int64_t snapshot_us;
} app_c15_busy_diag_t;

typedef struct {
    const char *branch;
    const char *response_code;
    int begin_result;
    bool busy_observed;
    bool busy_gate;
    bool begin_failure;
    const char *mapped_busy_source;
    int64_t busy_observed_us;
} app_c15_busy_event_t;

void app_c15_busy_diag_capture_locked(app_c15_busy_diag_t *out);
uint32_t app_c15_busy_diag_format_ops(const app_c15_busy_diag_t *diag,
                                      char *buf, size_t buflen,
                                      uint32_t *logged, bool *truncated);
void app_c15_busy_diag_format_job(const app_c15_busy_diag_t *diag,
                                  char *buf, size_t buflen);
void app_c15_busy_diag_capture(app_c15_busy_diag_t *out);
void app_c15_busy_diag_begin_snapshot(app_c15_begin_diag_t *out);
void app_c15_busy_diag_log(const app_c15_busy_diag_t *diag,
                           const app_c15_busy_event_t *event,
                           const app_c15_begin_diag_t *begin);
#endif
