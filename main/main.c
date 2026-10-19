#include <stdbool.h>

#include "esp_log.h"
#include "esp_wifi.h"
#include "nvs_flash.h"

#include "app_captive.h"
#include "bootstrap.h"
#include "bt.h"
#include "config.h"
#include "config_apply.h"
#include "data_pipe.h"
#include "diag.h"
#include "http.h"
#include "mqtt.h"
#include "ota.h"
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
    ESP_LOGI(TAG, "[boot-4c] config_apply_init");
    config_apply_init();

    /* 释放未选模式的 BT 栈内存(内部按 bt_mode 判断:BLE 常驻时自动跳过)。
     * 注:BLE 配网已按产品决策移除,无凭据设备改由 SoftAP 强制门户配网,
     * 因此不再需要"配网态保留整栈"的特殊分支。 */
    ESP_LOGI(TAG, "[boot-4b] app_bt_release_unused_memory");
    app_bt_release_unused_memory();
    ESP_LOGI(TAG, "[boot-5] app_configure_gpio");
    app_configure_gpio();
    ESP_LOGI(TAG, "[boot-6] app_ota_mount_fs");
    ESP_ERROR_CHECK(app_ota_mount_fs(true));
    ESP_LOGI(TAG, "[boot-7] app_start_wifi");
    app_start_wifi();
    ESP_LOGI(TAG, "[boot-7c] app_captive_init");
    app_captive_init();

    ESP_LOGI(TAG, "[boot-8] app_bt_apply_config");
    if (app_bt_apply_config() != ESP_OK) {
        char bt_last_error[64];
        app_bt_get_status(NULL, bt_last_error, sizeof(bt_last_error));
        ESP_LOGW(TAG, "Bluetooth init skipped (last_error=%s)", bt_last_error);
    }
    ESP_LOGI(TAG, "[boot-9] app_uart_apply_config");
    if (app_uart_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "UART init skipped");
    }
    /* 数据面:UART1 / BLE / MQTT 三边透传(BLE 端口由 transport_ble 注册,
     * MQTT 端口由 app_mqtt_start 注册;路由来自 NVS data_route_* 字段,1.3-07) */
    ESP_LOGI(TAG, "[boot-9b] data_pipe (uart1 / ble / mqtt)");
    if (data_pipe_init() == ESP_OK) {
        (void)data_pipe_register_port(DATA_PORT_UART1, app_uart_dataplane_write, NULL);
        data_pipe_set_enabled(true);
        (void)config_apply_domain_apply(CFG_DOMAIN_DATAPIPE);   /* 落位 NVS 路由 */
    } else {
        ESP_LOGW(TAG, "data_pipe init failed; passthrough disabled");
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
    /* 诊断:堆水位 + AP/STA 信道 + AP 客户端数,启动即打一行,之后每 30s 一行 */
    ESP_LOGI(TAG, "[boot-13b] app_diag_start");
    if (app_diag_start() != ESP_OK) {
        ESP_LOGW(TAG, "Diagnostics task init skipped");
    }

    ESP_LOGI(TAG, "iothub phase-4 ready: mode=%s, AP SSID='%s', open http://192.168.8.1 when AP is enabled",
             app_network_mode_to_string(s_config.net_mode),
             s_config.ap_ssid);
}
