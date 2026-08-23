#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bootstrap.h"
#include "common.h"
#include "config.h"
#include "constants.h"
#include "identity.h"
#include "ota.h"
#include "runtime.h"
#include "state.h"
#include "uart.h"
#include "wifi.h"

static const char *TAG = "bootstrap";

extern app_config_t s_config;
extern wifi_runtime_t s_wifi;

static void app_bootstrap_reset_runtime_state(void)
{
    app_uart_reset_runtime_state();
    app_copy_string(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), "0.0.0.0");
    app_copy_string(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), "0.0.0.0");
    app_copy_string(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), "0.0.0.0");
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
}

void app_bootstrap_set_defaults(void)
{
    app_config_set_defaults(&s_config);
    app_refresh_managed_identity_fields();
    app_bootstrap_reset_runtime_state();
}

esp_err_t app_bootstrap_save_config(void)
{
    return app_config_save(&s_config);
}

void app_bootstrap_load_config(void)
{
    app_bootstrap_reset_runtime_state();
    app_config_load(&s_config);
    app_refresh_managed_identity_fields();
}

static bool app_factory_reset_button_pressed(void)
{
    return gpio_get_level(APP_FACTORY_RESET_BOOT_GPIO) == 0;
}

static void app_factory_reset_to_defaults(void)
{
    ESP_LOGW(TAG, "BOOT/GPIO0 held for %d ms, restoring default configuration",
             APP_FACTORY_RESET_HOLD_MS);
    app_bootstrap_set_defaults();

    if (app_bootstrap_save_config() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save default configuration during factory reset");
        return;
    }

    ESP_LOGW(TAG, "Default configuration saved, rebooting device");
    if (xTaskCreate(app_reboot_task, "factory_reset_reboot", 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to schedule reboot after factory reset");
    }
}

static void app_factory_reset_button_task(void *arg)
{
    bool armed = true;
    TickType_t pressed_since = 0;
    const TickType_t poll_ticks = pdMS_TO_TICKS(APP_FACTORY_RESET_POLL_MS);
    const TickType_t hold_ticks = pdMS_TO_TICKS(APP_FACTORY_RESET_HOLD_MS);

    (void)arg;

    for (;;) {
        const bool pressed = app_factory_reset_button_pressed();

        if (!pressed) {
            pressed_since = 0;
            armed = true;
            vTaskDelay(poll_ticks);
            continue;
        }

        if (pressed_since == 0) {
            pressed_since = xTaskGetTickCount();
        }

        if (armed && (xTaskGetTickCount() - pressed_since) >= hold_ticks) {
            armed = false;
            app_factory_reset_to_defaults();
        }

        vTaskDelay(poll_ticks);
    }
}

esp_err_t app_factory_reset_start_task(void)
{
    return xTaskCreate(app_factory_reset_button_task, "factory_reset_btn", 3072, NULL, 5, NULL) == pdPASS
               ? ESP_OK
               : ESP_FAIL;
}
