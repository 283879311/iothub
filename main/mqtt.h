#pragma once

#include "esp_err.h"

#include "runtime.h"

extern mqtt_runtime_t s_mqtt;

esp_err_t app_mqtt_start(void);
esp_err_t app_mqtt_apply_config(void);
void app_mqtt_stop(void);
void app_mqtt_restart_task(void *arg);
bool app_mqtt_schedule_restart(void);
/* MQTT 是否正在做连接尝试(页面流避让用,见 问题与版本优化.md 1.3-36) */
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
bool app_mqtt_restart_is_deferred(void);
bool app_mqtt_restart_is_pending(void);
