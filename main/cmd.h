#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

#include "cmd_context.h"

/* ============================================================================
 * 命令内核(1.3-31 / 1.3-32 共用):MQTT RPC 与 BLE CMD 特征走同一分发器。
 * 内核只做"method 分发 + 统一层执行 + 结果组装",不感知 transport;
 * 限频/帧外壳(id/token/主题)由各 transport 自己处理。
 * 来源上下文定义见 cmd_context.h(C1 阶段 1)。
 * ========================================================================== */

typedef struct {
    bool reboot_after_response; /* effect=reboot:调用方必须先回包再延迟重启 */
    bool changed_any;           /* 有字段变化(触发 attributes/state 重发) */
    bool handled;               /* 帧内有可识别的方法/键(未知方法也算 handled) */
} app_cmd_result_t;

/* 执行一条命令(canonical 方法名 + 参数对象)。
 * 成功:resp_out 返回结果对象(调用方负责 cJSON_Delete),out 填 flags:
 *   - config.set/relay.set/led.set/io.set:{"success":true,"changed":[...],
 *     "effect":"none|subsys|reboot","relay_on":..,"led_on":..,"input_active":..,"io_seq":..}
 *   - 已删除共享属性顶层 relay_on/led_on 控制路径;平台控制只走 RPC 方法。
 *   - io.get:裸状态对象;config.get:裸脱敏配置对象(五段)
 * 失败:*resp_out=NULL,err_msg 指向静态错误串:
 *   unknown_method / guard_rejected / params_required / invalid_config /
 *   save_failed / domain_not_supported / out_of_memory */

/* 显式上下文入口(C1 阶段 1):来源校验失败(UNKNOWN/不变量破坏)拒绝执行,
 * 映射 internal_error。
 * 2026-11-05 v0 彻底删除:入参改为 (method, params) 直接传参,
 * 不再接受 v0 的 {"method","params"} 帧。 */
esp_err_t app_cmd_execute_method(const app_cmd_context_t *ctx, const char *method,
                                 const cJSON *params, cJSON **resp_out,
                                 app_cmd_result_t *out, const char **err_msg);

/* DEF-BLE-001:是否有在途网络操作(BLE 空闲回收用:配网在途时不得断链)。 */
bool app_cmd_net_op_active(void);

/* CMD-03 H1:网络 job/trial 模块初始化(须在 BLE/Wi-Fi 启动前调用一次)。 */
esp_err_t app_cmd_net_job_init(void);

/* BLE 断连:作废属于 BLE 会话的网络候选 trial,避免无会话时遗留可 apply 的凭据。 */
void app_cmd_net_on_ble_disconnect(void);
