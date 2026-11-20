#pragma once

#include "esp_err.h"

#include "runtime.h"

extern mqtt_runtime_t s_mqtt;

esp_err_t app_mqtt_start(void);
esp_err_t app_mqtt_apply_config(void);
void app_mqtt_stop(void);
void app_mqtt_restart_task(void *arg);
bool app_mqtt_schedule_restart(void);
/* MQTT 是否正在做连接尝试(页面流避让用) */
bool app_mqtt_connecting(void);
/* 清除致命错误位(配置变更入口调用,1.3-43) */
void app_mqtt_clear_fatal(void);
/* 出站队列深度(风暴观测,1.3-43) */
int app_mqtt_outbox_size(void);
/* 状态核心调用:请求尽快发布一次遥测 + client attributes(不阻塞调用者)。 */
void app_mqtt_on_io_change(void);
/* 1.3-44:因内存深紧张被跳过的周期遥测拍数(让路计数,diag 观测) */
uint32_t app_mqtt_tele_shed_count(void);
void app_mqtt_lock(void);
void app_mqtt_unlock(void);

/* G3: operation 活动期间延迟可重建型 MQTT restart。
 * begin 在已有 rebuild 正在执行时返回 ESP_ERR_INVALID_STATE；end 幂等。
 * 多个 schedule 请求在 defer 期间合并为一个 pending 请求；超过 15 秒自动放行。 */
esp_err_t app_mqtt_defer_restart_begin(void);
void app_mqtt_defer_restart_end(void);
/* 诊断版:多打一行 defer_end site=...,用于定位"谁提前放行重启"(DEF-MQTT-001)。 */
void app_mqtt_defer_restart_end_at(const char *site);
bool app_mqtt_restart_is_deferred(void);
/* DEF-MQTT-001:tryconnect worker 在第一次 WiFi 动作前调用 —— 先等受理回包
 * 发布(≤300ms),再等 broker PUBACK(≤1.2s),避免拆链早于回包确认。 */
void app_mqtt_resp_delivery_begin(void);
void app_mqtt_wait_resp_delivery(void);
bool app_mqtt_restart_is_pending(void);
/* DEF-NET-001(netcheck P2-3 修订):联网验证等待 MQTT 时用 —— 退避归零再
 * 拉起。高退避档(20/30s)下光 schedule 的 pending restart 仍要等完整档,
 * 验证窗口必超时;归零只由 mqtt.c 内写者维护(与 CONNECTED 复位同语义)。 */
void app_mqtt_poke_restart(void);
