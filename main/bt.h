#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"

extern uint8_t s_bt_runtime_mode;
extern char s_bt_last_error[64];

esp_err_t app_bt_apply_config(void);
void app_bt_restart_task(void *arg);
