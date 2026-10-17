#pragma once

#include "esp_err.h"

#include "runtime.h"

extern mqtt_runtime_t s_mqtt;

esp_err_t app_mqtt_start(void);
esp_err_t app_mqtt_apply_config(void);
void app_mqtt_stop(void);
void app_mqtt_restart_task(void *arg);
bool app_mqtt_schedule_restart(void);
/* MQTT 是否正在做连接尝试(页面流避让用,见 1.3版本优化.md 1.3-36) */
bool app_mqtt_connecting(void);
/* 清除致命错误位(配置变更入口调用,1.3-43) */
void app_mqtt_clear_fatal(void);
/* 出站队列深度(风暴观测,1.3-43) */
int app_mqtt_outbox_size(void);
/* 1.3-44:因内存深紧张被跳过的周期遥测拍数(让路计数,diag 观测) */
uint32_t app_mqtt_tele_shed_count(void);
void app_mqtt_lock(void);
void app_mqtt_unlock(void);
