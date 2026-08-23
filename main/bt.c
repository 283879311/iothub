#include <stdio.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bt.h"
#include "common.h"
#include "constants.h"

static const char *TAG = "app_bt";
static const char *SPP_SERVER_NAME = "IOTHUB_SPP";

extern app_config_t s_config;

uint8_t s_bt_runtime_mode = BT_MODE_OFF;
char s_bt_last_error[64] = "";

static bool s_ble_adv_configured = false;
static bool s_spp_ready = false;
static esp_bd_addr_t s_spp_remote_bda = {0};

static esp_ble_adv_params_t s_ble_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_adv_data_t s_ble_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static void app_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_MODE_CHG_EVT) {
        ESP_LOGI(TAG, "Classic BT mode changed: %d", param->mode_chg.mode);
    }
}

static void app_ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_ble_adv_configured = true;
        esp_ble_gap_start_advertising(&s_ble_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "BLE advertising started");
            app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "");
        } else {
            ESP_LOGE(TAG, "BLE advertising start failed: %d", param->adv_start_cmpl.status);
            snprintf(s_bt_last_error, sizeof(s_bt_last_error), "adv_start:%d", param->adv_start_cmpl.status);
            s_bt_runtime_mode = BT_MODE_OFF;
        }
        break;
    default:
        break;
    }
}

static void app_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVER_NAME);
        }
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            s_spp_ready = true;
            esp_bt_gap_set_device_name(s_config.bt_device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            ESP_LOGI(TAG, "SPP server started");
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        memcpy(s_spp_remote_bda, param->srv_open.rem_bda, sizeof(s_spp_remote_bda));
        ESP_LOGI(TAG, "SPP client connected");
        break;
    case ESP_SPP_CLOSE_EVT:
        memset(s_spp_remote_bda, 0, sizeof(s_spp_remote_bda));
        ESP_LOGI(TAG, "SPP client disconnected");
        break;
    default:
        break;
    }
}

static esp_err_t app_bt_init_stack(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;
    esp_err_t err;

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            return err;
        }
        ctrl_status = esp_bt_controller_get_status();
    }
    if (ctrl_status != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
        if (err != ESP_OK) {
            esp_bt_controller_deinit();
            return err;
        }
    }

    bluedroid_status = esp_bluedroid_get_status();
    if (bluedroid_status == ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
        if (err != ESP_OK) {
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            return err;
        }
        bluedroid_status = esp_bluedroid_get_status();
    }
    if (bluedroid_status != ESP_BLUEDROID_STATUS_ENABLED) {
        err = esp_bluedroid_enable();
        if (err != ESP_OK) {
            esp_bluedroid_deinit();
            esp_bt_controller_disable();
            esp_bt_controller_deinit();
            return err;
        }
    }
    return ESP_OK;
}

static void app_bt_cleanup_stack(void)
{
    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();

    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_disable();
    }
    if (bluedroid_status != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_deinit();
    }
    if (controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (controller_status != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
    }
}

static void app_bt_stop(void)
{
    if (s_bt_runtime_mode == BT_MODE_SPP && s_spp_ready) {
        esp_spp_deinit();
        s_spp_ready = false;
    }
    if (s_bt_runtime_mode == BT_MODE_BLE && s_ble_adv_configured) {
        esp_ble_gap_stop_advertising();
        s_ble_adv_configured = false;
    }

    app_bt_cleanup_stack();

    s_bt_runtime_mode = BT_MODE_OFF;
    app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "");
}

static esp_err_t app_bt_start_ble(void)
{
    esp_err_t err;

    err = app_bt_init_stack();
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_ble_gap_register_callback(app_ble_gap_cb);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "gap_callback:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_ble_gap_set_device_name(s_config.bt_device_name);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "set_name:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_ble_gap_config_adv_data(&s_ble_adv_data);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "adv_config:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    s_bt_runtime_mode = BT_MODE_BLE;
    app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "ble_starting");
    return ESP_OK;
}

static esp_err_t app_bt_start_spp(void)
{
    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};
    esp_err_t err;

    err = app_bt_init_stack();
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_bt_gap_register_callback(app_bt_gap_cb);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "bt_gap_register:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_spp_register_callback(app_spp_cb);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "spp_callback:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_spp_enhanced_init(&spp_cfg);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "spp_init:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    esp_bt_gap_set_pin(pin_type, 0, pin_code);
    s_bt_runtime_mode = BT_MODE_SPP;
    app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "");
    return ESP_OK;
}

esp_err_t app_bt_apply_config(void)
{
    app_bt_stop();

    if (s_config.bt_mode == BT_MODE_OFF) {
        return ESP_OK;
    }
    if (s_config.bt_mode == BT_MODE_BLE) {
        return app_bt_start_ble();
    }
    if (s_config.bt_mode == BT_MODE_SPP) {
        return app_bt_start_spp();
    }
    return ESP_ERR_INVALID_ARG;
}

void app_bt_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    if (app_bt_apply_config() != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth apply config failed");
    }
    vTaskDelete(NULL);
}
