#include "esp_log.h"
#include "nvs_flash.h"

#include "bootstrap.h"
#include "bt.h"
#include "config.h"
#include "http.h"
#include "mqtt.h"
#include "ota.h"
#include "sim.h"
#include "state.h"
#include "uart.h"
#include "wifi.h"

static const char *TAG = "iothub";

app_config_t s_config;

void app_main(void)
{
    esp_err_t err;

    esp_log_level_set("uart", ESP_LOG_DEBUG);
    esp_log_level_set("mqtt", ESP_LOG_DEBUG);
    app_uart_init();
    app_sim_init();

    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_bootstrap_load_config();
    app_configure_gpio();
    ESP_ERROR_CHECK(app_ota_mount_fs(true));
    app_start_wifi();
    if (app_bt_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth init skipped");
    }
    if (app_uart_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "UART init skipped");
    }
    if (app_mqtt_start() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT init skipped");
    }
    if (app_factory_reset_start_task() != ESP_OK) {
        ESP_LOGW(TAG, "Factory reset button task init skipped");
    }
    app_http_start_webserver();
    app_ota_confirm_running_image();

    ESP_LOGI(TAG, "iothub phase-4 ready: mode=%s, AP SSID='%s', open http://192.168.8.1 when AP is enabled",
             app_network_mode_to_string(s_config.net_mode),
             s_config.ap_ssid);
}
