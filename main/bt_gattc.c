#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#if !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#include "esp_gatt_common_api.h"
#include "esp_gatt_defs.h"
#include "esp_gattc_api.h"
#endif
#include "esp_log.h"

#include "bt_gattc.h"
#include "common.h"
#include "config.h"
#include "transport_ble.h"
/* 1.3-58 NimBLE Step 3:central 实现在 bt_gattc_nimble.c;本文件只构建
 * Bluedroid 版本。两者靠 sdkconfig 的 BT_HOST choice 编译期二选一。 */
#if !CONFIG_BT_NIMBLE_ENABLED

static const char *TAG = "bt_gattc";

#define GATTC_APP_ID   0x02
#define SCAN_DURATION  0 /* 0 = 持续扫描,找到目标即停 */

/* NUS RX/TX UUID(UUID 小端排列;与 bt_gatts.c 保持一致)
 * 注:主角色按"特征 UUID 直配"发现,不依赖服务边界,故无需服务 UUID */
static const uint8_t nus_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};
static const uint8_t nus_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};

static esp_gatt_if_t s_gattc_if = ESP_GATT_IF_NONE;
static esp_bd_addr_t s_remote_bda = {0};
static uint16_t s_conn_id = 0xFFFF;
static uint16_t s_rx_handle = 0; /* 对端 RX(我们写) */
static uint16_t s_tx_handle = 0; /* 对端 TX(我们订阅通知) */
static uint16_t s_tx_cccd_handle = 0;
static uint16_t s_mtu = 23;
static bool s_connected = false;
static bool s_notify_ready = false;
static bool s_scanning = false;

extern app_config_t s_config;

/* 扫描参数:主动扫描 + 接受任意地址类型 */
static esp_ble_scan_params_t s_scan_params = {
    .scan_type = BLE_SCAN_TYPE_ACTIVE,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .scan_filter_policy = BLE_SCAN_FILTER_ALLOW_ALL,
    .scan_interval = 0x50,
    .scan_window = 0x30,
    .scan_duplicate = BLE_SCAN_DUPLICATE_DISABLE,
};

/* 从广播数据里解析本地名(AD type 0x08/0x09) */
static bool app_ble_gattc_extract_name(const uint8_t *adv, uint8_t adv_len,
                                       char *out, size_t out_size)
{
    uint8_t i = 0;

    if (adv == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    while ((uint16_t)i + 1 < adv_len) {
        uint8_t field_len = adv[i];
        uint8_t type;

        if (field_len == 0) {
            break;
        }
        if ((uint16_t)i + 1 + field_len > adv_len) {
            break;
        }
        type = adv[i + 1];
        if ((type == 0x08 || type == 0x09) && field_len > 1) {
            size_t n = (size_t)(field_len - 1);
            if (n > out_size - 1) {
                n = out_size - 1;
            }
            memcpy(out, &adv[i + 2], n);
            out[n] = '\0';
            return true;
        }
        i = (uint8_t)(i + field_len + 1);
    }
    return false;
}

/* 目标判定:优先用配置的蓝牙名做前缀匹配;未配置则匹配 iothub 托管名前缀 */
static bool app_ble_gattc_name_matches(const uint8_t *adv, uint8_t adv_len)
{
    char name[40];

    if (!app_ble_gattc_extract_name(adv, adv_len, name, sizeof(name))) {
        return false;
    }
    if (s_config.bt_device_name[0] != '\0' &&
        strncmp(name, s_config.bt_device_name, strlen(s_config.bt_device_name)) == 0) {
        return true;
    }
    return (strncmp(name, "iot", 3) == 0) || (strncmp(name, "Iot", 3) == 0);
}

/* ---------------------------------------------------------------------------
 * GAP 回调(主角色独占):扫描参数就绪后开始扫描;发现目标 → 停止扫描并连接
 * ------------------------------------------------------------------------- */
static void app_ble_gattc_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_SCAN_PARAM_SET_COMPLETE_EVT:
        if (!s_connected && !s_scanning) {
            s_scanning = true;
            (void)esp_ble_gap_start_scanning(SCAN_DURATION);
            ESP_LOGI(TAG, "scanning for NUS peer (name prefix '%s')",
                     s_config.bt_device_name[0] ? s_config.bt_device_name : "iot*");
        }
        break;

    case ESP_GAP_BLE_SCAN_RESULT_EVT:
        if (param->scan_rst.search_evt != ESP_GAP_SEARCH_INQ_RES_EVT || s_connected) {
            break;
        }
        if (!app_ble_gattc_name_matches(param->scan_rst.ble_adv, param->scan_rst.adv_data_len)) {
            break;
        }
        (void)esp_ble_gap_stop_scanning();
        s_scanning = false;
        memcpy(s_remote_bda, param->scan_rst.bda, sizeof(esp_bd_addr_t));
        ESP_LOGI(TAG, "target found, connecting");
        (void)esp_ble_gattc_open(s_gattc_if, s_remote_bda, param->scan_rst.ble_addr_type, true);
        break;

    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * GATTC 回调:连接 → 协商 MTU → 发现特征 → 订阅通知 → 收数据
 * 硬约束:回调内不做耗时操作(通知只入队)
 * ------------------------------------------------------------------------- */
static void app_ble_gattc_event_handler(esp_gattc_cb_event_t event, esp_gatt_if_t gattc_if,
                                        esp_ble_gattc_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTC_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "app register failed: %d", param->reg.status);
            return;
        }
        s_gattc_if = gattc_if;
        (void)esp_ble_gatt_set_local_mtu(517);
        (void)esp_ble_gap_set_scan_params(&s_scan_params);
        break;

    case ESP_GATTC_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        memcpy(s_remote_bda, param->connect.remote_bda, sizeof(s_remote_bda));
        s_connected = true;
        ESP_LOGI(TAG, "connected (conn_id=%u), discovering", (unsigned)s_conn_id);
        (void)esp_ble_gattc_send_mtu_req(gattc_if, s_conn_id);
        (void)esp_ble_gattc_search_service(gattc_if, s_conn_id, NULL);
        break;

    case ESP_GATTC_CFG_MTU_EVT:
        if (param->cfg_mtu.status == ESP_GATT_OK) {
            s_mtu = param->cfg_mtu.mtu;
            ESP_LOGI(TAG, "mtu=%u (payload %u)", s_mtu,
                     (unsigned)((s_mtu > 3) ? (s_mtu - 3) : 20));
        }
        break;

    case ESP_GATTC_SEARCH_CMPL_EVT: {
        esp_gattc_char_elem_t chr[16];
        uint16_t count = 16;
        uint16_t i;

        if (esp_ble_gattc_get_all_char(gattc_if, s_conn_id, 0x0001, 0xFFFF, chr, &count, 0) != ESP_GATT_OK) {
            ESP_LOGW(TAG, "char discovery failed");
            break;
        }
        for (i = 0; i < count; i++) {
            if (chr[i].uuid.len != ESP_UUID_LEN_128) {
                continue;
            }
            if (memcmp(chr[i].uuid.uuid.uuid128, nus_rx_uuid, 16) == 0) {
                s_rx_handle = chr[i].char_handle;
            } else if (memcmp(chr[i].uuid.uuid.uuid128, nus_tx_uuid, 16) == 0) {
                s_tx_handle = chr[i].char_handle;
            }
        }
        ESP_LOGI(TAG, "NUS handles: rx=%u tx=%u", s_rx_handle, s_tx_handle);
        if (s_tx_handle != 0) {
            (void)esp_ble_gattc_register_for_notify(gattc_if, s_remote_bda, s_tx_handle);
        } else {
            ESP_LOGW(TAG, "peer has no NUS TX characteristic");
        }
        break;
    }

    case ESP_GATTC_REG_FOR_NOTIFY_EVT:
        if (param->reg_for_notify.status != ESP_GATT_OK) {
            ESP_LOGW(TAG, "register for notify failed: %d", param->reg_for_notify.status);
            break;
        }
        {
            esp_bt_uuid_t cccd;
            esp_gattc_descr_elem_t descr[8];
            uint16_t count = 8;

            memset(&cccd, 0, sizeof(cccd));
            cccd.len = ESP_UUID_LEN_16;
            cccd.uuid.uuid16 = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;
            if (esp_ble_gattc_get_descr_by_char_handle(gattc_if, s_conn_id,
                                                      param->reg_for_notify.handle,
                                                      cccd, descr, &count) == ESP_GATT_OK &&
                count > 0) {
                uint16_t notify_en = 1;
                s_tx_cccd_handle = descr[0].handle;
                (void)esp_ble_gattc_write_char_descr(gattc_if, s_conn_id, s_tx_cccd_handle,
                                                     sizeof(notify_en), (uint8_t *)&notify_en,
                                                     ESP_GATT_WRITE_TYPE_RSP, ESP_GATT_AUTH_REQ_NONE);
            }
        }
        break;

    case ESP_GATTC_WRITE_DESCR_EVT:
        if (param->write.status == ESP_GATT_OK && param->write.handle == s_tx_cccd_handle) {
            s_notify_ready = true;
            ESP_LOGI(TAG, "peer notify enabled (data plane up)");
        }
        break;

    case ESP_GATTC_NOTIFY_EVT:
        /* 对端通知:只入队(回调不得耗时) */
        if (param->notify.value_len > 0 && param->notify.value != NULL) {
            app_ble_dataplane_on_write(param->notify.value, param->notify.value_len);
        }
        break;

    case ESP_GATTC_DISCONNECT_EVT:
        ESP_LOGW(TAG, "disconnected (reason=0x%x)", (unsigned)param->disconnect.reason);
        s_connected = false;
        s_notify_ready = false;
        s_conn_id = 0xFFFF;
        s_rx_handle = 0;
        s_tx_handle = 0;
        s_tx_cccd_handle = 0;
        s_mtu = 23;
        if (!s_scanning) {
            s_scanning = true;
            (void)esp_ble_gap_start_scanning(SCAN_DURATION);
        }
        break;

    default:
        break;
    }
}

esp_err_t app_ble_gattc_start(void)
{
    esp_err_t err;

    err = esp_ble_gattc_register_callback(app_ble_gattc_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register gattc callback failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 主角色独占 GAP 扫描回调(bt.c 在 central 下不注册自有 GAP 回调) */
    err = esp_ble_gap_register_callback(app_ble_gattc_gap_cb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register gap callback failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gattc_app_register(GATTC_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app register failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void app_ble_gattc_reset(void)
{
    s_connected = false;
    s_notify_ready = false;
    s_scanning = false;
    s_conn_id = 0xFFFF;
    s_rx_handle = 0;
    s_tx_handle = 0;
    s_tx_cccd_handle = 0;
    s_mtu = 23;
}

bool app_ble_gattc_write(const uint8_t *data, size_t len, void *ctx)
{
    size_t offset = 0;
    uint16_t chunk;

    (void)ctx;
    if (!s_connected || s_rx_handle == 0 || data == NULL || len == 0) {
        return false;
    }
    chunk = (uint16_t)((s_mtu > 3) ? (s_mtu - 3) : 20);
    while (offset < len) {
        size_t n = len - offset;
        esp_err_t err;

        if (n > chunk) {
            n = chunk;
        }
        err = esp_ble_gattc_write_char(s_gattc_if, s_conn_id, s_rx_handle,
                                      (uint16_t)n, (uint8_t *)(data + offset),
                                      ESP_GATT_WRITE_TYPE_NO_RSP, ESP_GATT_AUTH_REQ_NONE);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "write failed: %s", esp_err_to_name(err));
            return offset > 0;
        }
        offset += n;
    }
    return true;
}

bool app_ble_gattc_connected(void)
{
    return s_connected;
}
#endif /* !CONFIG_BT_NIMBLE_ENABLED */
