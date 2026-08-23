#pragma once

#include "esp_err.h"

#include "runtime.h"

extern mqtt_runtime_t s_mqtt;

esp_err_t app_mqtt_start(void);
esp_err_t app_mqtt_apply_config(void);
void app_mqtt_stop(void);
void app_mqtt_restart_task(void *arg);
