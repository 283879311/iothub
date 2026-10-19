/* 1.3-58(NimBLE 迁移 Step 3):central / 主角色实现。
 *
 * 行为对齐 bt_gattc.c(Bluedroid):按名字前缀扫描 → 连接 → 发现 NUS RX/TX
 * 特征 → 订阅 TX(写 CCCD)→ 通知只入队;出站按 MTU 分片写对端 RX。
 * 生命周期与 bt.c 的 NimBLE 分支共用:host sync 后才允许扫描。
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"

#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bt_gattc.h"
#include "bt.h"
#include "common.h"
#include "config.h"
#include "transport_ble.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "host/ble_att.h"
#include "host/ble_gap.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_hs_adv.h"
#include "host/ble_uuid.h"
#include "os/os_mbuf.h"

static const char *TAG = "bt_gattc";

#define GATTC_CONNECT_TIMEOUT_MS 10000
#define GATTC_NOTIFY_BUF_MAX     512

/* 1.3-75:双板四特征压测固件。默认 0 = 生产 central(只做数据 RX/TX);
 * 测试构建改为 1:额外发现 CMD RX/TX、订阅两个 CCCD、按句柄分流通知,
 * 并在连接就绪后自动跑 RX/TX/CMD 三段压测(串口输出 TEST_PHASE/TEST_STAT)。 */
#ifndef APP_BLE_PEER_TEST
#define APP_BLE_PEER_TEST 0
#endif

/* NUS RX/TX UUID(小端排列;与 bt_gatts.c / bt_gatts_nimble.c 一致) */
static const uint8_t nus_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};
static const uint8_t nus_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};
#if APP_BLE_PEER_TEST
static const uint8_t nus_cmd_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x04, 0x00, 0x40, 0x6E
};
static const uint8_t nus_cmd_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x05, 0x00, 0x40, 0x6E
};
#endif

static uint16_t s_conn = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_rx_handle;
static uint16_t s_tx_handle;
static uint16_t s_cccd_handle;
static bool s_connected;
static bool s_connecting;   /* ble_gap_connect 在途:期间不得重起扫描 */
static bool s_notify_ready;
static bool s_scanning;
static bool s_resetting;   /* 拆栈中:断开事件不得再起扫描 */

#if APP_BLE_PEER_TEST
static uint16_t s_cmd_rx_handle;
static uint16_t s_cmd_tx_handle;
static uint16_t s_tx_cccd_handle;
static uint16_t s_cmd_tx_cccd_handle;
static bool s_tx_subscribed;
static bool s_cmd_tx_subscribed;
static bool s_peer_test_started;
static char s_cmd_token[48];
static uint32_t s_test_rx_writes;
static uint32_t s_test_rx_errors;
static uint32_t s_test_tx_notifies;
static uint32_t s_test_tx_bytes;
static uint32_t s_test_cmd_sent;
static uint32_t s_test_cmd_resp;
static uint32_t s_test_cmd_errors;
static uint32_t s_test_cmd_lat_sum;
static uint32_t s_test_cmd_lat_max;
static int64_t s_test_cmd_last_sent_ms;
#endif

/* 1.3-58: 调试辅助 —— 记录扫描到的前若干个不同广播名(去重、封顶),
 * 手机作为 NUS 从机时可据此确认"广播名是否被看见/是否匹配目标前缀" */
#define GATTC_SEEN_NAME_MAX 8
static char s_seen_names[GATTC_SEEN_NAME_MAX][32];
static uint8_t s_seen_count;

/* 扫描参数:主动扫描;interval 50ms / window 30ms(与 Bluedroid 侧 0x50/0x30 对齐) */
static const struct ble_gap_disc_params s_disc_params = {
    .itvl = 0x50,
    .window = 0x30,
    .filter_policy = 0,
    .limited = 0,
    .passive = 0,
    .filter_duplicates = 0,
};

/* 连接参数:40-80ms,latency 0,supervision 5s */
static const struct ble_gap_conn_params s_conn_params = {
    .scan_itvl = 0x50,
    .scan_window = 0x30,
    .itvl_min = 0x20,
    .itvl_max = 0x40,
    .latency = 0,
    .supervision_timeout = 500,
    .min_ce_len = 0,
    .max_ce_len = 0,
};

extern app_config_t s_config;

static int app_ble_gattc_gap_cb(struct ble_gap_event *event, void *arg);
static int app_ble_gattc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg);
static int app_ble_gattc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                                void *arg);
static int app_ble_gattc_cccd_written_cb(uint16_t conn_handle,
                                         const struct ble_gatt_error *error,
                                         struct ble_gatt_attr *attr, void *arg);
static int app_ble_gattc_mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                uint16_t mtu, void *arg);

#if APP_BLE_PEER_TEST
static void app_ble_gattc_peer_test_start(void);
static void app_ble_gattc_peer_test_task(void *arg);
static void app_ble_gattc_peer_cmd_rx(const uint8_t *data, uint16_t len);
#endif

/* 从广播数据里解析本地名(AD type 0x08/0x09) */
static bool app_ble_gattc_extract_name(const uint8_t *adv, uint8_t adv_len,
                                       char *out, size_t out_size)
{
    struct ble_hs_adv_fields fields;
    size_t n;

    if (adv == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (ble_hs_adv_parse_fields(&fields, adv, adv_len) != 0 ||
        fields.name == NULL || fields.name_len == 0) {
        return false;
    }
    n = fields.name_len;
    if (n > out_size - 1) {
        n = out_size - 1;
    }
    memcpy(out, fields.name, n);
    out[n] = '\0';
    return true;
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

static void app_ble_gattc_log_seen_name(const char *name)
{
    uint8_t i;

    if (name == NULL || name[0] == '\0') {
        return;
    }
    for (i = 0; i < s_seen_count; i++) {
        if (strcmp(s_seen_names[i], name) == 0) {
            return;
        }
    }
    if (s_seen_count >= GATTC_SEEN_NAME_MAX) {
        return;
    }
    app_copy_string(s_seen_names[s_seen_count], sizeof(s_seen_names[0]), name);
    s_seen_count++;
    ESP_LOGI(TAG, "scan seen: '%s'", name);
}

static void app_ble_gattc_start_scan(void)
{
    int rc;

    if (s_resetting || s_connected || s_connecting || s_scanning) {
        return;
    }
    rc = ble_gap_disc(BLE_OWN_ADDR_PUBLIC, BLE_HS_FOREVER, &s_disc_params,
                      app_ble_gattc_gap_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan start failed: %d", rc);
        return;
    }
    s_scanning = true;
    ESP_LOGI(TAG, "scanning for NUS peer (name prefix '%s')",
             s_config.bt_device_name[0] ? s_config.bt_device_name : "iot*");
}

/* ---- 特征发现:匹配 128-bit NUS RX/TX ---- */
static int app_ble_gattc_chr_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                const struct ble_gatt_chr *chr, void *arg)
{
    (void)conn_handle;
    (void)arg;

    if (error->status == 0 && chr != NULL) {
        if (chr->uuid.u.type == BLE_UUID_TYPE_128) {
            if (memcmp(chr->uuid.u128.value, nus_rx_uuid, 16) == 0) {
                s_rx_handle = chr->val_handle;
            } else if (memcmp(chr->uuid.u128.value, nus_tx_uuid, 16) == 0) {
                s_tx_handle = chr->val_handle;
            }
#if APP_BLE_PEER_TEST
            else if (memcmp(chr->uuid.u128.value, nus_cmd_rx_uuid, 16) == 0) {
                s_cmd_rx_handle = chr->val_handle;
            } else if (memcmp(chr->uuid.u128.value, nus_cmd_tx_uuid, 16) == 0) {
                s_cmd_tx_handle = chr->val_handle;
            }
#endif
        }
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
#if APP_BLE_PEER_TEST
        ESP_LOGI(TAG, "NUS handles: rx=%u tx=%u cmd_rx=%u cmd_tx=%u",
                 s_rx_handle, s_tx_handle, s_cmd_rx_handle, s_cmd_tx_handle);
#else
        ESP_LOGI(TAG, "NUS handles: rx=%u tx=%u", s_rx_handle, s_tx_handle);
#endif
        if (s_tx_handle != 0) {
            int rc = ble_gattc_disc_all_dscs(s_conn, s_tx_handle, 0xFFFF,
                                             app_ble_gattc_dsc_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "descriptor discovery failed: %d", rc);
            }
        } else {
            ESP_LOGW(TAG, "peer has no NUS TX characteristic");
        }
        return 0;
    }
    ESP_LOGW(TAG, "char discovery error: %d", error->status);
    return 0;
}

/* ---- 描述符发现:找 TX 的 CCCD(0x2902) ---- */
static int app_ble_gattc_dsc_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                uint16_t chr_val_handle, const struct ble_gatt_dsc *dsc,
                                void *arg)
{
    (void)conn_handle;
    (void)chr_val_handle;
    (void)arg;

    if (error->status == 0 && dsc != NULL) {
        /* 1.3-60: 只认第一个 0x2902。发现范围是 TX value→0xFFFF,会一路扫到
         * 后续特征的 CCCD;若每次覆盖,最终订阅的是 CMD TX(实测 handle=10),
         * 数据 TX 反而没订阅。第一个才是 TX 紧邻的 CCCD。 */
#if APP_BLE_PEER_TEST
        if (dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            if (s_tx_cccd_handle == 0) {
                s_tx_cccd_handle = dsc->handle;
            } else if (s_cmd_tx_cccd_handle == 0) {
                s_cmd_tx_cccd_handle = dsc->handle;
            }
        }
#else
        if (s_cccd_handle == 0 &&
            dsc->uuid.u.type == BLE_UUID_TYPE_16 && dsc->uuid.u16.value == 0x2902) {
            s_cccd_handle = dsc->handle;
        }
#endif
        return 0;
    }
    if (error->status == BLE_HS_EDONE) {
#if APP_BLE_PEER_TEST
        ESP_LOGI(TAG, "CCCDs: tx=%u cmd_tx=%u", s_tx_cccd_handle, s_cmd_tx_cccd_handle);
        if (s_tx_cccd_handle != 0) {
            uint16_t notify_en = 1;
            int rc = ble_gattc_write_flat(s_conn, s_tx_cccd_handle,
                                          &notify_en, sizeof(notify_en),
                                          app_ble_gattc_cccd_written_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "TX CCCD write start failed: %d", rc);
            }
        }
        if (s_cmd_tx_cccd_handle != 0) {
            uint16_t notify_en = 1;
            int rc = ble_gattc_write_flat(s_conn, s_cmd_tx_cccd_handle,
                                          &notify_en, sizeof(notify_en),
                                          app_ble_gattc_cccd_written_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "CMD TX CCCD write start failed: %d", rc);
            }
        }
#else
        if (s_cccd_handle != 0) {
            uint16_t notify_en = 1;   /* little-endian: notify */
            int rc = ble_gattc_write_flat(s_conn, s_cccd_handle,
                                          &notify_en, sizeof(notify_en),
                                          app_ble_gattc_cccd_written_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "CCCD write start failed: %d", rc);
            }
        } else {
            ESP_LOGW(TAG, "peer TX has no CCCD");
        }
#endif
        return 0;
    }
    ESP_LOGW(TAG, "descriptor discovery error: %d", error->status);
    return 0;
}

/* ---- CCCD 写完成:通知通道就绪 ---- */
static int app_ble_gattc_cccd_written_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                         struct ble_gatt_attr *attr, void *arg)
{
    (void)conn_handle;
    (void)attr;
    (void)arg;

    if (error->status == 0) {
#if APP_BLE_PEER_TEST
        if (attr != NULL && attr->handle == s_tx_cccd_handle) {
            s_tx_subscribed = true;
        } else if (attr != NULL && attr->handle == s_cmd_tx_cccd_handle) {
            s_cmd_tx_subscribed = true;
        }
        s_notify_ready = true;
        ESP_LOGI(TAG, "CCCD write ok: handle=%u tx_sub=%d cmd_sub=%d",
                 attr != NULL ? attr->handle : 0,
                 (int)s_tx_subscribed, (int)s_cmd_tx_subscribed);
        if (s_tx_subscribed && s_cmd_tx_subscribed) {
            app_ble_gattc_peer_test_start();
        }
#else
        s_notify_ready = true;
        ESP_LOGI(TAG, "peer notify enabled (data plane up)");
#endif
    } else {
        ESP_LOGW(TAG, "CCCD write failed: %d", error->status);
    }
    return 0;
}

/* ---- MTU 协商完成:继续特征发现 ---- */
static int app_ble_gattc_mtu_cb(uint16_t conn_handle, const struct ble_gatt_error *error,
                                uint16_t mtu, void *arg)
{
    int rc;

    (void)arg;
    if (error->status == 0) {
        ESP_LOGI(TAG, "mtu=%u (payload %u)", mtu,
                 (unsigned)((mtu > 3) ? (mtu - 3) : 20));
    } else {
        ESP_LOGW(TAG, "mtu exchange failed: %d (continue with %u)",
                 error->status, (unsigned)ble_att_mtu(conn_handle));
    }
    rc = ble_gattc_disc_all_chrs(conn_handle, 0x0001, 0xFFFF,
                                 app_ble_gattc_chr_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "char discovery start failed: %d", rc);
        (void)ble_gap_terminate(conn_handle, BLE_ERR_REM_USER_CONN_TERM);
    }
    return 0;
}

/* ---- GAP 事件:扫描结果 / 连接 / 断开 / 通知 ---- */
static int app_ble_gattc_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_DISC: {
        int rc;
        char name[40];

        if (s_connected) {
            break;
        }
        if (!app_ble_gattc_name_matches(event->disc.data, event->disc.length_data)) {
            if (app_ble_gattc_extract_name(event->disc.data, event->disc.length_data,
                                           name, sizeof(name))) {
                app_ble_gattc_log_seen_name(name);
            }
            break;
        }
        (void)ble_gap_disc_cancel();
        s_scanning = false;
        s_connecting = true;
        ESP_LOGI(TAG, "target found, connecting");
        rc = ble_gap_connect(BLE_OWN_ADDR_PUBLIC, &event->disc.addr,
                             GATTC_CONNECT_TIMEOUT_MS, &s_conn_params,
                             app_ble_gattc_gap_cb, NULL);
        if (rc != 0) {
            ESP_LOGW(TAG, "connect start failed: %d", rc);
            s_connecting = false;
            app_ble_gattc_start_scan();
        }
        break;
    }

    case BLE_GAP_EVENT_CONNECT:
        s_connecting = false;
        if (event->connect.status == 0) {
            int rc;

            s_connected = true;
            s_conn = event->connect.conn_handle;
            app_bt_status_clear_error();   /* 1.3-64:已连上,清掉 central_scanning */
            ESP_LOGI(TAG, "connected (conn=%u), negotiating mtu", s_conn);
            /* NimBLE 不会自动交换 MTU(Bluedroid 侧是 send_mtu_req),必须显式发起 */
            rc = ble_gattc_exchange_mtu(s_conn, app_ble_gattc_mtu_cb, NULL);
            if (rc != 0) {
                ESP_LOGW(TAG, "mtu exchange start failed: %d (mtu=%u)",
                         rc, (unsigned)ble_att_mtu(s_conn));
                rc = ble_gattc_disc_all_chrs(s_conn, 0x0001, 0xFFFF,
                                             app_ble_gattc_chr_cb, NULL);
                if (rc != 0) {
                    ESP_LOGW(TAG, "char discovery start failed: %d", rc);
                    (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
                }
            }
        } else {
            ESP_LOGW(TAG, "connect failed: %d", event->connect.status);
            app_ble_gattc_start_scan();
        }
        break;

    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGW(TAG, "disconnected (reason=0x%x)", (unsigned)event->disconnect.reason);
        s_connected = false;
        s_connecting = false;
        s_notify_ready = false;
        s_conn = BLE_HS_CONN_HANDLE_NONE;
        s_rx_handle = 0;
        s_tx_handle = 0;
        s_cccd_handle = 0;
#if APP_BLE_PEER_TEST
        s_cmd_rx_handle = 0;
        s_cmd_tx_handle = 0;
        s_tx_cccd_handle = 0;
        s_cmd_tx_cccd_handle = 0;
        s_tx_subscribed = false;
        s_cmd_tx_subscribed = false;
        s_cmd_token[0] = '\0';
#endif
        app_ble_gattc_start_scan();
        break;

    case BLE_GAP_EVENT_NOTIFY_RX: {
        uint8_t buf[GATTC_NOTIFY_BUF_MAX];
        int pkt_len = OS_MBUF_PKTLEN(event->notify_rx.om);
        uint16_t n;

        if (pkt_len <= 0) {
            break;
        }
        n = (uint16_t)(pkt_len > (int)sizeof(buf) ? sizeof(buf) : pkt_len);
        if (os_mbuf_copydata(event->notify_rx.om, 0, n, buf) == 0) {
#if APP_BLE_PEER_TEST
            if (event->notify_rx.attr_handle == s_tx_handle) {
                s_test_tx_notifies++;
                s_test_tx_bytes += n;
                app_ble_dataplane_on_write(buf, n);
            } else if (event->notify_rx.attr_handle == s_cmd_tx_handle) {
                app_ble_gattc_peer_cmd_rx(buf, n);
            }
#else
            /* 回调只入队,绝不阻塞(与 Bluedroid 侧同一纪律) */
            app_ble_dataplane_on_write(buf, n);
#endif
        }
        break;
    }

    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "mtu=%u (payload %u)", event->mtu.value,
                 (unsigned)((event->mtu.value > 3) ? (event->mtu.value - 3) : 20));
        break;

    case BLE_GAP_EVENT_DISC_COMPLETE:
        s_scanning = false;
        if (!s_connected && !s_connecting && !s_resetting) {
            app_ble_gattc_start_scan();
        }
        break;

    default:
        break;
    }
    return 0;
}

#if APP_BLE_PEER_TEST
static void app_ble_gattc_peer_cmd_rx(const uint8_t *data, uint16_t len)
{
    char buf[256];
    size_t n = len < sizeof(buf) - 1 ? len : sizeof(buf) - 1;
    cJSON *root;
    const cJSON *tok = NULL;

    if (data == NULL || len == 0) {
        return;
    }
    memcpy(buf, data, n);
    buf[n] = '\0';
    root = cJSON_Parse(buf);
    if (!cJSON_IsObject(root)) {
        s_test_cmd_errors++;
        if (root != NULL) {
            cJSON_Delete(root);
        }
        return;
    }
    {
        const cJSON *node = root;
        int depth;

        /* B 的 CMD 壳会把 login 响应再包一层:
         * {"id":0,"ok":true,"result":{"result":{"token":"..."}}}
         * 这里递归向下找 token(最多 4 层)。 */
        for (depth = 0; depth < 4 && cJSON_IsObject(node); depth++) {
            tok = cJSON_GetObjectItemCaseSensitive(node, "token");
            if (cJSON_IsString(tok)) {
                break;
            }
            node = cJSON_GetObjectItemCaseSensitive(node, "result");
        }
    }
    if (cJSON_IsString(tok) && tok->valuestring != NULL) {
        app_copy_string(s_cmd_token, sizeof(s_cmd_token), tok->valuestring);
    } else if (s_cmd_token[0] == '\0') {
        ESP_LOGW(TAG, "CMD RX no token: %.96s", buf);
    }
    s_test_cmd_resp++;
    if (s_test_cmd_last_sent_ms > 0) {
        int64_t now = esp_timer_get_time() / 1000;
        uint32_t lat = (uint32_t)(now - s_test_cmd_last_sent_ms);
        s_test_cmd_lat_sum += lat;
        if (lat > s_test_cmd_lat_max) {
            s_test_cmd_lat_max = lat;
        }
    }
    cJSON_Delete(root);
}

static void app_ble_gattc_peer_cmd_send(const char *line)
{
    int rc;

    if (line == NULL || s_cmd_rx_handle == 0) {
        return;
    }
    rc = ble_gattc_write_no_rsp_flat(s_conn, s_cmd_rx_handle, line,
                                     (uint16_t)strlen(line));
    if (rc == 0) {
        s_test_cmd_sent++;
        s_test_cmd_last_sent_ms = esp_timer_get_time() / 1000;
    } else {
        s_test_cmd_errors++;
    }
}

static void app_ble_gattc_peer_phase_rx(uint32_t secs)
{
    uint8_t frame[20];
    int64_t end = esp_timer_get_time() / 1000 + (int64_t)secs * 1000;

    s_test_rx_writes = 0;
    s_test_rx_errors = 0;
    ESP_LOGI(TAG, "TEST_PHASE=rx secs=%u", (unsigned)secs);
    while (esp_timer_get_time() / 1000 < end && s_connected && s_tx_subscribed) {
        size_t i;
        int rc;

        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = (uint8_t)(s_test_rx_writes + i);
        }
        rc = ble_gattc_write_no_rsp_flat(s_conn, s_rx_handle, frame, sizeof(frame));
        if (rc == 0) {
            s_test_rx_writes++;
        } else {
            s_test_rx_errors++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "TEST_STAT phase=rx writes=%u errors=%u",
             (unsigned)s_test_rx_writes, (unsigned)s_test_rx_errors);
}

static void app_ble_gattc_peer_phase_tx(uint32_t secs)
{
    uint8_t frame[20];
    int64_t end = esp_timer_get_time() / 1000 + (int64_t)secs * 1000;
    uint32_t writes = 0;
    uint32_t errors = 0;

    s_test_tx_notifies = 0;
    s_test_tx_bytes = 0;
    ESP_LOGI(TAG, "TEST_PHASE=tx secs=%u (需要从机 loopback ON)", (unsigned)secs);
    while (esp_timer_get_time() / 1000 < end && s_connected && s_tx_subscribed) {
        size_t i;
        int rc;

        for (i = 0; i < sizeof(frame); i++) {
            frame[i] = (uint8_t)(writes + i);
        }
        rc = ble_gattc_write_no_rsp_flat(s_conn, s_rx_handle, frame, sizeof(frame));
        if (rc == 0) {
            writes++;
        } else {
            errors++;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    ESP_LOGI(TAG, "TEST_STAT phase=tx writes=%u errors=%u notifies=%u bytes=%u",
             (unsigned)writes, (unsigned)errors,
             (unsigned)s_test_tx_notifies, (unsigned)s_test_tx_bytes);
}

static void app_ble_gattc_peer_phase_cmd(uint32_t secs)
{
    int64_t end = esp_timer_get_time() / 1000 + (int64_t)secs * 1000;
    int64_t login_deadline = esp_timer_get_time() / 1000 + 2000;

    s_test_cmd_sent = 0;
    s_test_cmd_resp = 0;
    s_test_cmd_errors = 0;
    s_test_cmd_lat_sum = 0;
    s_test_cmd_lat_max = 0;
    s_test_cmd_last_sent_ms = 0;
    s_cmd_token[0] = '\0';
    ESP_LOGI(TAG, "TEST_PHASE=cmd secs=%u (需要从机 loopback OFF)", (unsigned)secs);
    app_ble_gattc_peer_cmd_send(
        "{\"method\":\"login\",\"params\":{\"username\":\"admin\",\"password\":\"123456\"},\"token\":\"\"}\n");
    while (esp_timer_get_time() / 1000 < login_deadline &&
           s_connected && s_cmd_token[0] == '\0') {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if (s_cmd_token[0] == '\0') {
        ESP_LOGW(TAG, "TEST_LOGIN=timeout");
    }
    while (esp_timer_get_time() / 1000 < end && s_connected && s_cmd_token[0] != '\0') {
        char line[128];

        snprintf(line, sizeof(line),
                 "{\"method\":\"getStatus\",\"params\":{},\"token\":\"%s\"}\n",
                 s_cmd_token);
        app_ble_gattc_peer_cmd_send(line);
        vTaskDelay(pdMS_TO_TICKS(500));   /* 1.3-76:CMD 压测降到 2 次/秒 */
    }
    ESP_LOGI(TAG, "TEST_STAT phase=cmd sent=%u resp=%u err=%u lat_avg=%u lat_max=%u",
             (unsigned)s_test_cmd_sent, (unsigned)s_test_cmd_resp,
             (unsigned)s_test_cmd_errors,
             s_test_cmd_resp > 0 ? (unsigned)(s_test_cmd_lat_sum / s_test_cmd_resp) : 0,
             (unsigned)s_test_cmd_lat_max);
}

static void app_ble_gattc_peer_test_task(void *arg)
{
    uint32_t cycle = 0;

    (void)arg;
    for (;;) {
        if (!s_connected || !s_tx_subscribed || !s_cmd_tx_subscribed) {
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        cycle++;
        ESP_LOGI(TAG, "TEST_CYCLE=%u", (unsigned)cycle);
        app_ble_gattc_peer_phase_rx(60);
        if (!s_connected) {
            continue;
        }
        app_ble_gattc_peer_phase_tx(60);
        if (!s_connected) {
            continue;
        }
        app_ble_gattc_peer_phase_cmd(60);
    }
}

static void app_ble_gattc_peer_test_start(void)
{
    if (s_peer_test_started) {
        return;
    }
    s_peer_test_started = true;
    if (xTaskCreate(app_ble_gattc_peer_test_task, "ble_peer_test", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "peer test task create failed");
        s_peer_test_started = false;
    } else {
        ESP_LOGI(TAG, "peer test task started (RX/TX/CMD 60s each)");
    }
}
#endif /* APP_BLE_PEER_TEST */

esp_err_t app_ble_gattc_start(void)
{
    s_resetting = false;
    s_connected = false;
    s_connecting = false;
    s_notify_ready = false;
    s_scanning = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_rx_handle = 0;
    s_tx_handle = 0;
    s_cccd_handle = 0;
    memset(s_seen_names, 0, sizeof(s_seen_names));
    s_seen_count = 0;
    /* 扫描由 app_ble_gattc_nimble_on_sync() 在 host sync 后启动 */
    return ESP_OK;
}

void app_ble_gattc_nimble_on_sync(void)
{
    app_ble_gattc_start_scan();
}

void app_ble_gattc_reset(void)
{
    s_resetting = true;
    if (s_scanning) {
        (void)ble_gap_disc_cancel();
        s_scanning = false;
    }
    if (s_connected && s_conn != BLE_HS_CONN_HANDLE_NONE) {
        (void)ble_gap_terminate(s_conn, BLE_ERR_REM_USER_CONN_TERM);
    }
    s_connected = false;
    s_connecting = false;
    s_notify_ready = false;
    s_conn = BLE_HS_CONN_HANDLE_NONE;
    s_rx_handle = 0;
    s_tx_handle = 0;
    s_cccd_handle = 0;
}

bool app_ble_gattc_write(const uint8_t *data, size_t len, void *ctx)
{
    size_t offset = 0;
    uint16_t chunk;

    (void)ctx;
    if (!s_connected || s_conn == BLE_HS_CONN_HANDLE_NONE || s_rx_handle == 0 ||
        data == NULL || len == 0) {
        return false;
    }
    chunk = ble_att_mtu(s_conn);
    chunk = (uint16_t)((chunk > 3) ? (chunk - 3) : 20);
    while (offset < len) {
        size_t n = len - offset;
        int rc;

        if (n > chunk) {
            n = chunk;
        }
        rc = ble_gattc_write_no_rsp_flat(s_conn, s_rx_handle, data + offset, (uint16_t)n);
        if (rc != 0) {
            ESP_LOGW(TAG, "write failed: %d", rc);
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

#endif /* CONFIG_BT_NIMBLE_ENABLED */
