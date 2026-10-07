#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_log.h"
#include "esp_spp_api.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bt.h"
#include "common.h"
#include "constants.h"

static const char *TAG = "app_bt";
static const char *SPP_SERVER_NAME = "IOTHUB_SPP";

extern app_config_t s_config;

/* 运行态由自旋锁保护:写入方包括 BT 栈回调线程,读取方包括 Web 线程,
 * 临界区内只有定长拷贝,任意上下文均安全。 */
static uint8_t s_bt_runtime_mode = BT_MODE_OFF;
static char s_bt_last_error[64] = "";
static portMUX_TYPE s_bt_status_mux = portMUX_INITIALIZER_UNLOCKED;

/* 蓝牙协议栈是否真实启动过。mode=off 时 app_bt_release_unused_memory() 已把
 * 整个 _bt_bss 段(含 libbt 的全部状态变量,如 bluedroid 的 s_bt_host_state)
 * 还给堆,此后任何 libbt/controller 的状态查询读到的都是垃圾值,
 * 必须以本标记为准决定是否触碰栈 API,否则会在 OFF 首配时崩溃(见 问题.md 第 19 项)。 */
static bool s_stack_started = false;

static void app_bt_status_set_mode(uint8_t mode)
{
    portENTER_CRITICAL(&s_bt_status_mux);
    s_bt_runtime_mode = mode;
    portEXIT_CRITICAL(&s_bt_status_mux);
}

static void app_bt_status_set_error(const char *error)
{
    portENTER_CRITICAL(&s_bt_status_mux);
    app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), error ? error : "");
    portEXIT_CRITICAL(&s_bt_status_mux);
}

static void app_bt_status_set_errorf(const char *fmt, ...)
{
    char buf[sizeof(s_bt_last_error)];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);
    app_bt_status_set_error(buf);
}

static uint8_t app_bt_status_get_mode(void)
{
    uint8_t mode;
    portENTER_CRITICAL(&s_bt_status_mux);
    mode = s_bt_runtime_mode;
    portEXIT_CRITICAL(&s_bt_status_mux);
    return mode;
}

void app_bt_get_status(uint8_t *runtime_mode, char *last_error_buf, size_t last_error_buf_size)
{
    portENTER_CRITICAL(&s_bt_status_mux);
    if (runtime_mode != NULL) {
        *runtime_mode = s_bt_runtime_mode;
    }
    if (last_error_buf != NULL && last_error_buf_size > 0) {
        app_copy_string(last_error_buf, last_error_buf_size, s_bt_last_error);
    }
    portEXIT_CRITICAL(&s_bt_status_mux);
}

/* 开机一次性:按配置的蓝牙模式释放“永不使用”的那套栈内存。
 * OFF 释放 BTDM 全部;BLE 释放 Classic;SPP 释放 BLE。
 * 被释放的模式在本上电周期内不可再启用。SPP 长时间运行后堆碎片化会使栈
 * 热重配(反复 deinit/init)初始化失败(问题.md 第 21 项),因此任何蓝牙
 * 配置修改一律保存后整机重启,不做运行期热重配。 */
void app_bt_release_unused_memory(void)
{
    static bool s_released = false;
    uint8_t bt_mode;
    size_t free_before;
    size_t free_after;
    esp_err_t err;

    if (s_released) {
        return;
    }
    s_released = true;

    app_config_lock();
    bt_mode = s_config.bt_mode;
    app_config_unlock();

    free_before = (size_t)esp_get_free_heap_size();
    switch (bt_mode) {
    case BT_MODE_BLE:
        err = esp_bt_mem_release(ESP_BT_MODE_CLASSIC_BT);
        break;
    case BT_MODE_SPP:
        err = esp_bt_mem_release(ESP_BT_MODE_BLE);
        break;
    case BT_MODE_OFF:
    default:
        err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
        break;
    }
    free_after = (size_t)esp_get_free_heap_size();

    if (err == ESP_OK) {
        ESP_LOGI(TAG, "BT unused-mode memory released (mode=%s, heap +%u bytes)",
                 app_bt_mode_to_string(bt_mode),
                 (unsigned)(free_after - free_before));
    } else {
        ESP_LOGW(TAG, "BT mem release skipped (mode=%s): %s",
                 app_bt_mode_to_string(bt_mode), esp_err_to_name(err));
    }
}

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
            app_bt_status_set_error("");
        } else {
            ESP_LOGE(TAG, "BLE advertising start failed: %d", param->adv_start_cmpl.status);
            app_bt_status_set_errorf("adv_start:%d", param->adv_start_cmpl.status);
            app_bt_status_set_mode(BT_MODE_OFF);
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

static esp_err_t app_bt_init_stack(esp_bt_mode_t bt_mode)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;
    esp_err_t err;

    /* esp_bt_controller_enable(mode) 要求 mode 与 init 时的 cfg->mode 一致
     * (IDF bt.c 历史约束);BTDM 构建的默认模板是 ESP_BT_MODE_BTDM,
     * 不按目标模式覆写则 enable 恒返回 ESP_ERR_INVALID_ARG(问题.md 第 23 项) */
    bt_cfg.mode = bt_mode;

    ctrl_status = esp_bt_controller_get_status();
    if (ctrl_status == ESP_BT_CONTROLLER_STATUS_IDLE) {
        err = esp_bt_controller_init(&bt_cfg);
        if (err != ESP_OK) {
            return err;
        }
        ctrl_status = esp_bt_controller_get_status();
    }
    if (ctrl_status != ESP_BT_CONTROLLER_STATUS_ENABLED) {
        err = esp_bt_controller_enable(bt_mode);
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
    s_stack_started = true;
    return ESP_OK;
}

static void app_bt_cleanup_stack(void)
{
    esp_bluedroid_status_t bluedroid_status;
    esp_bt_controller_status_t controller_status;

    /* 栈从未启动(OFF 首配):_bt_bss 已被释放,状态查询与任何栈 API 都不可触碰 */
    if (!s_stack_started) {
        return;
    }
    s_stack_started = false;

    bluedroid_status = esp_bluedroid_get_status();
    controller_status = esp_bt_controller_get_status();

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
    uint8_t runtime_mode = app_bt_status_get_mode();

    if (runtime_mode == BT_MODE_SPP && s_spp_ready) {
        esp_spp_deinit();
        s_spp_ready = false;
    }
    if (runtime_mode == BT_MODE_BLE && s_ble_adv_configured) {
        esp_ble_gap_stop_advertising();
        s_ble_adv_configured = false;
    }

    app_bt_cleanup_stack();

    app_bt_status_set_mode(BT_MODE_OFF);
    /* 不在此处清空 last_error:启动失败路径的错误信息靠它带出(问题.md 第 23 项) */
}

static esp_err_t app_bt_start_ble(void)
{
    esp_err_t err;

    err = app_bt_init_stack(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_ble_gap_register_callback(app_ble_gap_cb);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("gap_callback:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_ble_gap_set_device_name(s_config.bt_device_name);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("set_name:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_ble_gap_config_adv_data(&s_ble_adv_data);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("adv_config:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    app_bt_status_set_mode(BT_MODE_BLE);
    app_bt_status_set_error("ble_starting");
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

    err = app_bt_init_stack(ESP_BT_MODE_CLASSIC_BT);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_bt_gap_register_callback(app_bt_gap_cb);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("bt_gap_register:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_spp_register_callback(app_spp_cb);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("spp_callback:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    err = esp_spp_enhanced_init(&spp_cfg);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("spp_init:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    esp_bt_gap_set_pin(pin_type, 0, pin_code);
    app_bt_status_set_mode(BT_MODE_SPP);
    app_bt_status_set_error("");
    return ESP_OK;
}

esp_err_t app_bt_apply_config(void)
{
    app_bt_stop();

    if (s_config.bt_mode == BT_MODE_OFF) {
        app_bt_status_set_error("");
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
