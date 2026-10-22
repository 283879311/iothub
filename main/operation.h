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
} app_operation_error_t;

typedef struct app_operation app_operation_t;

/* run_fn 由单 worker 调用；它负责推进 connecting→got_ip 等阶段，
 * 不得保存口令。返回非 ESP_OK 时模块把仍处于活动态的 operation 置为 failed。 */
typedef esp_err_t (*app_operation_run_fn)(app_operation_t *op, void *ctx);

/* TTL/取消时的清理钩子：返回 false 表示清理失败，operation 进入 failed
 * 而不是 expired/cancelled，避免"operation 已过期但底层 trial 仍可提交"。 */
typedef bool (*app_operation_cleanup_fn)(app_operation_t *op, void *ctx);

typedef struct {
    char id[APP_OPERATION_ID_LEN];
    char idem_key[APP_OPERATION_IDEM_KEY_LEN];
    app_operation_owner_t owner;
    app_op_state_t state;
    app_operation_error_t error;
    int64_t created_ms;
    int64_t deadline_ms;
    int64_t terminal_ms;
    uint8_t generation;
} app_operation_status_t;

/* 静态表初始化；不创建 worker，不占堆。 */
esp_err_t app_operation_init(void);

/* 创建 operation 入队并唤醒唯一 worker。accepted_ttl_ms/got_ip_ttl_ms 传 0
 * 使用默认值(20s/300s)。并发的第二个网络操作返回 ESP_ERR_INVALID_STATE。 */
esp_err_t app_operation_start(app_operation_owner_t owner, const char *idem_key,
                              uint32_t accepted_ttl_ms, uint32_t got_ip_ttl_ms,
                              app_operation_run_fn run_fn,
                              app_operation_cleanup_fn cleanup_fn,
                              void *ctx, app_operation_t **out);

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
