#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "esp_err.h"

/* ============================================================================
 * 命令内核(1.3-31 / 1.3-32 共用):MQTT RPC 与 BLE CMD 特征走同一分发器。
 * 内核只做"method 分发 + 统一层执行 + 结果组装",不感知 transport;
 * 限频/帧外壳(id/token/主题)由各 transport 自己处理。
 * ========================================================================== */

typedef struct {
    bool reboot_after_response; /* effect=reboot:调用方必须先回包再延迟重启 */
    bool changed_any;           /* 有字段变化(触发 attributes/state 重发) */
    bool handled;               /* 帧内有可识别的方法/键(未知方法也算 handled) */
} app_cmd_result_t;

/* 执行命令帧 {"method":...,"params":...}(兼容顶层 relay_on/led_on 键,共享属性写法)。
 * 成功:resp_out 返回结果对象(调用方负责 cJSON_Delete),out 填 flags:
 *   - setConfig/setRelay/setLed/setState/顶层键:{"success":true,"changed":[...],
 *     "effect":"none|subsys|reboot","relay_on":..,"led_on":..}
 *   - getStatus:裸状态对象;getConfig:裸脱敏配置对象(五段)
 * 失败:*resp_out=NULL,err_msg 指向静态错误串:
 *   unknown_method / guard_rejected / params_required / invalid_config /
 *   save_failed / domain_not_supported / out_of_memory */
esp_err_t app_cmd_execute(const cJSON *frame, cJSON **resp_out, app_cmd_result_t *out,
                          const char **err_msg);
