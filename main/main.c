#include "esp_log.h"
#include "esp_wifi.h"
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
    ESP_LOGI(TAG, "[boot-1] app_uart_init");
    app_uart_init();
    ESP_LOGI(TAG, "[boot-2] app_sim_init");
    app_sim_init();

    ESP_LOGI(TAG, "[boot-3] nvs_flash_init");
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_LOGW(TAG, "[boot-3a] erasing NVS and reinit");
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);
    {
        esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (storage_err != ESP_OK && storage_err != ESP_ERR_WIFI_NOT_INIT) {
            ESP_LOGW(TAG, "[boot-3b] pre-set wifi storage RAM failed: %s", esp_err_to_name(storage_err));
        }
    }

    ESP_LOGI(TAG, "[boot-4] app_bootstrap_load_config");
    app_bootstrap_load_config();
    ESP_LOGI(TAG, "[boot-5] app_configure_gpio");
    app_configure_gpio();
    ESP_LOGI(TAG, "[boot-6] app_ota_mount_fs");
    ESP_ERROR_CHECK(app_ota_mount_fs(true));
    ESP_LOGI(TAG, "[boot-7] app_start_wifi");
    app_start_wifi();
    ESP_LOGI(TAG, "[boot-8] app_bt_apply_config");
    if (app_bt_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth init skipped");
    }
    ESP_LOGI(TAG, "[boot-9] app_uart_apply_config");
    if (app_uart_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "UART init skipped");
    }
    ESP_LOGI(TAG, "[boot-10] app_mqtt_start");
    if (app_mqtt_start() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT init skipped");
    }
    ESP_LOGI(TAG, "[boot-11] app_factory_reset_start_task");
    if (app_factory_reset_start_task() != ESP_OK) {
        ESP_LOGW(TAG, "Factory reset button task init skipped");
    }
    ESP_LOGI(TAG, "[boot-12] app_http_start_webserver");
    app_http_start_webserver();
    ESP_LOGI(TAG, "[boot-13] app_ota_confirm_running_image");
    app_ota_confirm_running_image();

    ESP_LOGI(TAG, "iothub phase-4 ready: mode=%s, AP SSID='%s', open http://192.168.8.1 when AP is enabled",
             app_network_mode_to_string(s_config.net_mode),
             s_config.ap_ssid);
}
