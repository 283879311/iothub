#include <stdbool.h>

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bootstrap.h"
#include "common.h"
#include "config.h"
#include "constants.h"
#include "identity.h"
#include "ota.h"
#include "prov_pin.h"
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
    app_copy_string(s_wifi.connected_ssid, sizeof(s_wifi.connected_ssid), "");
    s_wifi.connected_profile = -1;
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
    if (app_config_scrub_legacy_ap(&s_config)) {
        esp_err_t scrub_err = app_bootstrap_save_config();

        if (scrub_err == ESP_OK) {
            ESP_LOGI(TAG, "legacy AP config bytes scrubbed from NVS layout");
        } else {
            ESP_LOGW(TAG, "legacy AP config scrub save failed: %s",
                     esp_err_to_name(scrub_err));
        }
    }
    app_refresh_managed_identity_fields();
}

static bool app_factory_reset_button_pressed(void)
{
    return gpio_get_level(APP_FACTORY_RESET_BOOT_GPIO) == 0;
}

static bool app_factory_reset_to_defaults(void)
{
    ESP_LOGW(TAG, "BOOT/GPIO0 held for %d ms, restoring default configuration",
             APP_FACTORY_RESET_HOLD_MS);
    app_bootstrap_set_defaults();
    app_config_clear_output_last();

    esp_err_t profile_err = app_config_clear_wifi_profiles();
    if (profile_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear saved Wi-Fi profiles: %s",
                 esp_err_to_name(profile_err));
        return false;
    }
    esp_err_t prov_err = app_prov_state_clear();
    if (prov_err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to clear provisioning PIN/state: %s",
                 esp_err_to_name(prov_err));
        return false;
    }

    /* 同时清掉 Wi-Fi 驱动可能写入 NVS 的配置(本工程平时用 RAM storage,
     * 但官方配网或其他代码路径可能留下)。失败只告警,不阻塞主配置重置。 */
    esp_err_t wifi_err = esp_wifi_restore();
    if (wifi_err != ESP_OK && wifi_err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGW(TAG, "Failed to restore Wi-Fi driver config: %s",
                 esp_err_to_name(wifi_err));
    }

    if (app_bootstrap_save_config() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to save default configuration during factory reset");
        return false;
    }

    ESP_LOGW(TAG, "Default configuration saved; release BOOT to reboot");
    return true;
}

static void app_factory_reset_button_task(void *arg)
{
    bool armed = true;
    bool reboot_pending = false;
    bool blink_active = false;
    bool previous_led_on = false;
    bool blink_level = false;
    TickType_t pressed_since = 0;
    TickType_t blink_since = 0;
    const TickType_t poll_ticks = pdMS_TO_TICKS(APP_FACTORY_RESET_POLL_MS);
    const TickType_t hold_ticks = pdMS_TO_TICKS(APP_FACTORY_RESET_HOLD_MS);
    const TickType_t blink_ticks = pdMS_TO_TICKS(250);

    (void)arg;

    for (;;) {
        const bool pressed = app_factory_reset_button_pressed();
        const TickType_t now = xTaskGetTickCount();

        if (!pressed) {
            pressed_since = 0;
            armed = true;
            if (blink_active) {
                /* 松开后先恢复原 LED 状态，再安排重启。 */
                blink_active = false;
                (void)app_io_set_outputs(false, false, true, previous_led_on,
                                         APP_IO_SOURCE_BOOT);
            }
            if (reboot_pending) {
                /* 等按键释放后再重启：GPIO0 仍为低时复位会进入 ROM 下载模式。 */
                reboot_pending = false;
                ESP_LOGW(TAG, "BOOT released, rebooting after factory reset");
                app_reboot_schedule("factory_reset_reboot");
            }
            vTaskDelay(poll_ticks);
            continue;
        }

        if (pressed_since == 0) {
            pressed_since = now;
        }

        if (armed && (now - pressed_since) >= hold_ticks) {
            app_io_state_t io;

            armed = false;
            app_io_get_state(&io);
            previous_led_on = io.led_on;
            reboot_pending = app_factory_reset_to_defaults();

            /* 配置已切到默认策略(固定关闭)，闪烁不会写 LED 上次状态 NVS。 */
            blink_active = true;
            blink_level = !previous_led_on;
            blink_since = now;
            (void)app_io_set_outputs(false, false, true, blink_level,
                                     APP_IO_SOURCE_BOOT);
        }

        if (blink_active && (now - blink_since) >= blink_ticks) {
            blink_since = now;
            blink_level = !blink_level;
            (void)app_io_set_outputs(false, false, true, blink_level,
                                     APP_IO_SOURCE_BOOT);
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
