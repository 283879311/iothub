#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "operation_state.h"

#define APP_OPERATION_MAX_SLOTS 4
#define APP_OPERATION_ID_LEN 20
#define APP_OPERATION_IDEM_KEY_LEN 65

/* operation 记录不保存 Wi-Fi 口令/MQTT token；owner 只用于断连/权限语义。 */
typedef enum {
    APP_OPERATION_OWNER_INTERNAL = 0,
    APP_OPERATION_OWNER_BLE,
    APP_OPERATION_OWNER_HTTP,
    APP_OPERATION_OWNER_MQTT,
} app_operation_owner_t;

typedef enum {
    APP_OPERATION_ERR_NONE = 0,
    APP_OPERATION_ERR_BUSY,
    APP_OPERATION_ERR_NO_SLOT,
    APP_OPERATION_ERR_INTERNAL,
    APP_OPERATION_ERR_CLEANUP_FAILED,
    APP_OPERATION_ERR_TIMEOUT,
    APP_OPERATION_ERR_CANCELLED,
    APP_OPERATION_ERR_NOT_FOUND,
    APP_OPERATION_ERR_WRONG_PASSWORD,
    APP_OPERATION_ERR_AP_NOT_FOUND,
    APP_OPERATION_ERR_CONNECT_FAILED,
    APP_OPERATION_ERR_TRIAL_INVALID,
    APP_OPERATION_ERR_OPERATION_STATE,
    APP_OPERATION_ERR_STORAGE_FAILED,
    APP_OPERATION_ERR_ROLLBACK_FAILED,
    APP_OPERATION_ERR_SCAN_FAILED,   /* P1-10:候选预扫描两次失败,未进入连接 */
    APP_OPERATION_ERR_WIFI_CONFIG_OFFLINE, /* M1 R9:离线门控拒绝 tryconnect */
    APP_OPERATION_ERR_MQTT_UNAVAILABLE,
    APP_OPERATION_ERR_INTERNET_UNVERIFIED,
    APP_OPERATION_ERR_STATIC_IP_PINNED,
    APP_OPERATION_ERR_LAST_PROFILE,
    /* DEF-NET-001 选 B 条件 2:tryconnect 验证等待上限到期(25s 具名常量),
     * 判决未出炉即拆链收尾——不是成功、不是无限等 */
    APP_OPERATION_ERR_VERIFY_TIMEOUT,
} app_operation_error_t;

typedef struct app_operation app_operation_t;

/* run_fn 由单 worker 调用；它负责推进 connecting→got_ip 等阶段，
 * 不得保存口令。返回非 ESP_OK 时模块把仍处于活动态的 operation 置为 failed。 */
typedef esp_err_t (*app_operation_run_fn)(app_operation_t *op, void *ctx);

/* TTL/取消时的清理钩子：返回 false 表示清理失败，operation 进入 failed
 * 而不是 expired/cancelled，避免"operation 已过期但底层 trial 仍可提交"。
 * 兼容旧路径；新 finish 路径使用 policy-aware finish_fn。 */
typedef bool (*app_operation_cleanup_fn)(app_operation_t *op, void *ctx);

/* P1：finish policy。COMMITTING 不作为 policy，由 finish API 直接拒绝。 */
typedef enum {
    APP_OPERATION_CLEANUP_REQUIRED = 0,
    APP_OPERATION_CLEANUP_RELEASE_ONLY,
    APP_OPERATION_CLEANUP_NO_RESOURCE_RELEASE,
} app_operation_cleanup_policy_t;

/* finish 路径的 cleanup/release 结果。 */
typedef enum {
    APP_OPERATION_CLEANUP_OUTCOME_DONE_OK = 0,
    APP_OPERATION_CLEANUP_OUTCOME_DONE_FAIL,
    APP_OPERATION_CLEANUP_OUTCOME_PENDING,
    APP_OPERATION_CLEANUP_OUTCOME_NO_JOB,
    APP_OPERATION_CLEANUP_OUTCOME_STALE,
} app_operation_cleanup_outcome_t;

typedef enum {
    APP_OPERATION_FINISH_DONE = 0,
    APP_OPERATION_FINISH_PENDING,
    APP_OPERATION_FINISH_REJECTED,
    APP_OPERATION_FINISH_STALE,
} app_operation_finish_result_t;

/* policy-aware finish 回调：在 s_ops_mux 外执行。
 * expected_generation 由 operation finish intent 传入，供资源释放前复核。 */
typedef app_operation_cleanup_outcome_t (*app_operation_finish_fn)(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation);

/* CMD-03b：cleanup 一次性认领状态。
 * NONE 未认领；RUNNING 已有认领者执行中；OK/FAIL 认领者已完成。
 * 随 operation 槽位 generation 复位，不跨槽位复用保留。 */
typedef enum {
    APP_OPERATION_CLEANUP_NONE = 0,
    APP_OPERATION_CLEANUP_RUNNING,
    APP_OPERATION_CLEANUP_OK,
    APP_OPERATION_CLEANUP_FAIL,
} app_operation_cleanup_state_t;

typedef struct {
    char id[APP_OPERATION_ID_LEN];
    char idem_key[APP_OPERATION_IDEM_KEY_LEN];
    app_operation_owner_t owner;
    app_op_state_t state;
    app_operation_error_t error;
    int64_t created_ms;
    int64_t deadline_ms;
    int64_t terminal_ms;
    uint32_t generation;
    app_operation_cleanup_state_t cleanup_state;
} app_operation_status_t;

typedef enum {
    APP_OPERATION_CLEANUP_CLAIMED = 0,
    APP_OPERATION_CLEANUP_ALREADY_RUNNING,
    APP_OPERATION_CLEANUP_ALREADY_DONE_OK,
    APP_OPERATION_CLEANUP_ALREADY_DONE_FAIL,
    APP_OPERATION_CLEANUP_INVALID,
    APP_OPERATION_CLEANUP_STALE,
} app_operation_cleanup_claim_t;

#ifdef APP_C15_BUSY_DIAG
/* 仅 BUSY 诊断构建：在 s_ops_mux 内一次性复制 operation 表；
 * 只读，不 reclaim、不调用回调、不改变状态。 */
typedef struct {
    char id[APP_OPERATION_ID_LEN];
    uint8_t state;
    bool running;
    uint32_t generation;
    uint8_t cleanup_state;
} app_operation_diag_op_t;

typedef struct {
    uint32_t active_ops;
    uint32_t printed;
    bool truncated;
    bool job_op_found;
    app_operation_diag_op_t job_op;
    app_operation_diag_op_t ops[APP_OPERATION_MAX_SLOTS];
} app_operation_diag_t;

bool app_operation_diag_snapshot(app_operation_diag_t *out,
                                 const app_operation_t *job_op);
#endif

/* 以下 API 只在 s_ops_mux 内读写 claim；不改变 operation 状态迁移语义。
 * claim 准入：槽位有效、generation 匹配、state 为活动态、claim==NONE；
 * expiring/running 不作为拒绝条件（P4/P5 的正常入口）。 */
bool app_operation_generation_matches(const app_operation_t *op, uint32_t expected_generation);
app_operation_cleanup_claim_t app_operation_cleanup_claim(app_operation_t *op,
                                                          uint32_t expected_generation);
void app_operation_cleanup_complete(app_operation_t *op, uint32_t expected_generation, bool ok);
app_operation_cleanup_state_t app_operation_cleanup_result(const app_operation_t *op,
                                                           uint32_t expected_generation);

/* 静态表初始化；不创建 worker，不占堆。 */
esp_err_t app_operation_init(void);

/* 创建 operation 入队并唤醒唯一 worker。accepted_ttl_ms/got_ip_ttl_ms 传 0
 * 使用默认值(20s/300s)。并发的第二个网络操作返回 ESP_ERR_INVALID_STATE。
 * 终态保留统一 600s（同 key 重试在保留期内幂等返回原终态）；
 * 槽满且无活动 operation 时覆盖最老终态记录，正常不再返回 ESP_ERR_NO_MEM。 */
esp_err_t app_operation_start(app_operation_owner_t owner, const char *idem_key,
                              uint32_t accepted_ttl_ms, uint32_t got_ip_ttl_ms,
                              app_operation_run_fn run_fn,
                              app_operation_cleanup_fn cleanup_fn,
                              void *ctx, app_operation_t **out);

/* P1：带 policy-aware finish 回调的启动入口。旧 app_operation_start 等价
 * finish_fn=NULL；未迁移路径继续使用旧 cleanup_fn。 */
esp_err_t app_operation_start_with_finish(app_operation_owner_t owner, const char *idem_key,
                                          uint32_t accepted_ttl_ms, uint32_t got_ip_ttl_ms,
                                          app_operation_run_fn run_fn,
                                          app_operation_cleanup_fn cleanup_fn,
                                          app_operation_finish_fn finish_fn,
                                          void *ctx, app_operation_t **out);

/* 统一终态请求：first-writer 在 s_ops_mux 内原子登记；callback 在锁外执行。
 * 第一个请求执行 policy 并 finalize；后续请求返回 PENDING，不执行 callback。 */
app_operation_finish_result_t
app_operation_request_finish(app_operation_t *op, uint32_t expected_generation,
                             app_op_state_t target_state,
                             app_operation_error_t target_error,
                             app_operation_cleanup_policy_t cleanup_policy);

esp_err_t app_operation_set_state(app_operation_t *op, app_op_state_t state,
                                  app_operation_error_t error);
esp_err_t app_operation_cancel(app_operation_t *op);
bool app_operation_get_status(const app_operation_t *op, app_operation_status_t *out);
const char *app_operation_id(const app_operation_t *op);
app_op_state_t app_operation_state(const app_operation_t *op);
app_operation_t *app_operation_find(const char *operation_id);
app_operation_t *app_operation_find_by_key(app_operation_owner_t owner, const char *idem_key);
const char *app_operation_error_name(app_operation_error_t error);

#ifdef APP_OPERATION_SELFTEST
void app_operation_selftest_start(void);
void app_operation_set_test_retention_ms(uint32_t ms);
#endif

#ifdef APP_OPERATION_LIFECYCLE_TEST
void app_operation_lifecycle_test_start(void);
#endif

/* ---------------------------------------------------------------------------
 * 实验构建专用(C1 复审确定性钩子,APP_C1_TEST_HOOKS=ON 才编译):
 * 武装后,下一次 cancel/expire 的 cleanup 返回、finalize 之前会暂停
 * hold_ms 毫秒;期间可用 probe_to 对同一 op 直接调 set_state 验证
 * "expiring 闸拒绝一切迁移"。正式构建不含这些符号。
 * ------------------------------------------------------------------------- */
#ifdef APP_C1_TEST_HOOKS
void app_operation_test_hook_arm(uint32_t hold_ms, int probe_to_state);
void app_operation_test_hook_clear(void);
#endif
