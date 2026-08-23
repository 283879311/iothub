#pragma once

#include "esp_err.h"

void app_bootstrap_set_defaults(void);
esp_err_t app_bootstrap_save_config(void);
void app_bootstrap_load_config(void);
esp_err_t app_factory_reset_start_task(void);
