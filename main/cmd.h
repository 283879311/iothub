#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/* ============================================================================
 * 命令内核(1.3-31 / 1.3-32 共用):MQTT RPC 与 BLE CMD 特征走同一分发器。
 * 内核只做"method 分发 + 统一层执行 + 结果组装",不感知 transport;
 * 限频/帧外壳(id/token/主题)由各 transport 自己处理。
 * ========================================================================== */

/* 命令入口来源:仅用于状态审计/上报,不改变鉴权语义(各 transport 自己鉴权)。 */
typedef enum {
    APP_CMD_SOURCE_MQTT = 0,
    APP_CMD_SOURCE_BLE,
    APP_CMD_SOURCE_HTTP,
    APP_CMD_SOURCE_TOUCH,
} app_cmd_source_t;

typedef struct {
    bool reboot_after_response; /* effect=reboot:调用方必须先回包再延迟重启 */
    bool changed_any;           /* 有字段变化(触发 attributes/state 重发) */
    bool handled;               /* 帧内有可识别的方法/键(未知方法也算 handled) */
} app_cmd_result_t;

/* 执行命令帧 {"method":...,"params":...}。
 * 成功:resp_out 返回结果对象(调用方负责 cJSON_Delete),out 填 flags:
 *   - setConfig/setRelay/setLed/setState:{"success":true,"changed":[...],
 *     "effect":"none|subsys|reboot","relay_on":..,"led_on":..,"input_active":..,"io_seq":..}
 *   - 已删除共享属性顶层 relay_on/led_on 控制路径;平台控制只走 RPC 方法。
 *   - getStatus:裸状态对象;getConfig:裸脱敏配置对象(五段)
 * 失败:*resp_out=NULL,err_msg 指向静态错误串:
 *   unknown_method / guard_rejected / params_required / invalid_config /
 *   save_failed / domain_not_supported / out_of_memory */
esp_err_t app_cmd_execute_ex(const cJSON *frame, app_cmd_source_t source,
                             cJSON **resp_out, app_cmd_result_t *out,
                             const char **err_msg);

/* BLE 断连:作废属于 BLE 会话的网络候选 trial,避免无会话时遗留可 apply 的凭据。 */
void app_cmd_net_on_ble_disconnect(void);
