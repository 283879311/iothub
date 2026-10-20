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
#include "esp_gatts_api.h"
#endif
#include "esp_log.h"

#include "bt.h"
#include "ble_cmd.h"
#include "bt_gatts.h"
#include "data_pipe.h"
#include "transport_ble.h"

#if !CONFIG_BT_NIMBLE_ENABLED

static const char *TAG = "bt_gatts";

#define GATTS_APP_ID       0x01
#define GATTS_SERVICE_INST 0

/* ---------------------------------------------------------------------------
 * 1.3-46 特征值瘦身:应用侧数组只是"初值模板",容量在属性表里用 max_length 声明。
 * 依据(IDF 6.0.2 Bluedroid 源码实证):
 *   - 建表时栈按 max_length **自建堆副本**(gatt_db.c:563 osi_malloc(attr_max_len)
 *     + memcpy 初值);写请求落地的是这份堆副本(gatt_sr.c:632),应用数组此后再不被读写;
 *   - 读请求返回 attr_len(=0),不是 max_length ⇒ notify-only 特征值声明 256B
 *     等于白占"256B BSS + 256B 堆副本"(读恒空、写无权限)。
 * 故:模板统一 1 字节占位;RX 类 max_length=BLE_RX_VALUE_MAX(写落地容量),
 * notify 类 max_length=1(占位)。省 4×255B BSS + 2×255B 堆副本。
 * ------------------------------------------------------------------------- */
#define BLE_RX_VALUE_MAX   256 /* 单次写入落地容量(必须 ≥ DATA_PIPE_CHUNK_MAX,见下方断言) */
#define BLE_TX_VALUE_MAX   1   /* notify-only 占位:读返回长度 0,写无权限 */

/* 跨模块不变量:数据面按 DATA_PIPE_CHUNK_MAX(256B)分块喂入本端对端写,
 * RX 属性值容量一旦小于它,大帧会在 GATT 写这一层被静默拒绝(数据丢失)。 */
_Static_assert(BLE_RX_VALUE_MAX >= DATA_PIPE_CHUNK_MAX,
               "RX 属性值容量必须 ≥ 数据面单块上限,否则大帧在 GATT 写层被拒");

/* Nordic UART Service(UUID 小端排列):
 *   Service 6E400001-B5A3-F393-E0A9-E50E24DCCA9E
 *   RX      6E400002-... (Write / WriteWithoutResponse)   纯数据
 *   TX      6E400003-... (Notify)                         纯数据
 *   CMD RX  6E400004-... (Write)                          命令帧(1.3-32)
 *   CMD TX  6E400005-... (Notify)                         命令回包 */
static const uint8_t nus_service_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E
};
static const uint8_t nus_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E
};
static const uint8_t nus_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E
};
static const uint8_t nus_cmd_rx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x04, 0x00, 0x40, 0x6E
};
static const uint8_t nus_cmd_tx_uuid[16] = {
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x05, 0x00, 0x40, 0x6E
};

/* 属性表索引 */
enum {
    IDX_SVC = 0,
    IDX_CHAR_RX,
    IDX_CHAR_RX_VAL,
    IDX_CHAR_TX,
    IDX_CHAR_TX_VAL,
    IDX_CHAR_TX_CFG,
    IDX_CHAR_CMDRX,
    IDX_CHAR_CMDRX_VAL,
    IDX_CHAR_CMDTX,
    IDX_CHAR_CMDTX_VAL,
    IDX_CHAR_CMDTX_CFG,
    IDX_NB
};

static const uint16_t primary_service_uuid = ESP_GATT_UUID_PRI_SERVICE;
static const uint16_t char_declare_uuid = ESP_GATT_UUID_CHAR_DECLARE;
static const uint16_t char_cccd_uuid = ESP_GATT_UUID_CHAR_CLIENT_CONFIG;

static uint8_t rx_props = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static uint8_t tx_props = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t tx_cccd[2] = {0x00, 0x00};
static uint8_t cmd_rx_props = ESP_GATT_CHAR_PROP_BIT_WRITE | ESP_GATT_CHAR_PROP_BIT_WRITE_NR;
static uint8_t cmd_tx_props = ESP_GATT_CHAR_PROP_BIT_NOTIFY;
static uint8_t cmd_tx_cccd[2] = {0x00, 0x00};
/* 值模板(见上方 1.3-46 说明):容量由属性表 max_length 声明,这里只保证指针有效 */
static uint8_t s_value_tmpl[1] = {0};

/* 官方推荐:用属性表一次性建服务,免逐条 add_char */
static const esp_gatts_attr_db_t s_gatt_db[IDX_NB] = {
    [IDX_SVC] = {{ESP_GATT_AUTO_RSP},
                 {ESP_UUID_LEN_16, (uint8_t *)&primary_service_uuid, ESP_GATT_PERM_READ,
                  sizeof(nus_service_uuid), sizeof(nus_service_uuid), (uint8_t *)nus_service_uuid}},
    [IDX_CHAR_RX] = {{ESP_GATT_AUTO_RSP},
                     {ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
                      sizeof(rx_props), sizeof(rx_props), &rx_props}},
    [IDX_CHAR_RX_VAL] = {{ESP_GATT_AUTO_RSP},
                         {ESP_UUID_LEN_128, (uint8_t *)nus_rx_uuid, ESP_GATT_PERM_WRITE,
                          BLE_RX_VALUE_MAX, 0, s_value_tmpl}},
    [IDX_CHAR_TX] = {{ESP_GATT_AUTO_RSP},
                     {ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
                      sizeof(tx_props), sizeof(tx_props), &tx_props}},
    [IDX_CHAR_TX_VAL] = {{ESP_GATT_AUTO_RSP},
                         {ESP_UUID_LEN_128, (uint8_t *)nus_tx_uuid, ESP_GATT_PERM_READ,
                          BLE_TX_VALUE_MAX, 0, s_value_tmpl}},
    [IDX_CHAR_TX_CFG] = {{ESP_GATT_AUTO_RSP},
                         {ESP_UUID_LEN_16, (uint8_t *)&char_cccd_uuid,
                          ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                          sizeof(tx_cccd), sizeof(tx_cccd), tx_cccd}},
    [IDX_CHAR_CMDRX] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
                         sizeof(cmd_rx_props), sizeof(cmd_rx_props), &cmd_rx_props}},
    [IDX_CHAR_CMDRX_VAL] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_128, (uint8_t *)nus_cmd_rx_uuid, ESP_GATT_PERM_WRITE,
                             BLE_RX_VALUE_MAX, 0, s_value_tmpl}},
    [IDX_CHAR_CMDTX] = {{ESP_GATT_AUTO_RSP},
                        {ESP_UUID_LEN_16, (uint8_t *)&char_declare_uuid, ESP_GATT_PERM_READ,
                         sizeof(cmd_tx_props), sizeof(cmd_tx_props), &cmd_tx_props}},
    [IDX_CHAR_CMDTX_VAL] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_128, (uint8_t *)nus_cmd_tx_uuid, ESP_GATT_PERM_READ,
                             BLE_TX_VALUE_MAX, 0, s_value_tmpl}},
    [IDX_CHAR_CMDTX_CFG] = {{ESP_GATT_AUTO_RSP},
                            {ESP_UUID_LEN_16, (uint8_t *)&char_cccd_uuid,
                             ESP_GATT_PERM_READ | ESP_GATT_PERM_WRITE,
                             sizeof(cmd_tx_cccd), sizeof(cmd_tx_cccd), cmd_tx_cccd}},
};

static esp_gatt_if_t s_gatts_if = ESP_GATT_IF_NONE;
static uint16_t s_handles[IDX_NB];
static uint16_t s_conn_id = 0xFFFF;
static uint16_t s_mtu = 23;
static bool s_connected = false;
static bool s_congested = false;
/* 1.3-03 诊断:写事件限频日志(前 6 条全打,其后每 20 条一条)+ 被条件挡掉的写也要可见 */
static uint32_t s_write_seen = 0;
static uint32_t s_write_logged = 0;
static uint32_t s_write_ignored = 0;
static bool s_started = false;

/* ---------------------------------------------------------------------------
 * GATT 事件回调
 * 硬约束(官方):回调内不得做耗时操作 —— 写入只入队,发送只在 data_pipe 任务里做
 * ------------------------------------------------------------------------- */
static void app_ble_gatts_event_handler(esp_gatts_cb_event_t event, esp_gatt_if_t gatts_if,
                                        esp_ble_gatts_cb_param_t *param)
{
    switch (event) {
    case ESP_GATTS_REG_EVT:
        if (param->reg.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "app register failed: %d", param->reg.status);
            return;
        }
        s_gatts_if = gatts_if;
        /* 主动请求大 MTU(实际以协商结果为准,见 ESP_GATTS_MTU_EVT) */
        (void)esp_ble_gatt_set_local_mtu(517);
        (void)esp_ble_gatts_create_attr_tab(s_gatt_db, gatts_if, IDX_NB, GATTS_SERVICE_INST);
        break;

    case ESP_GATTS_CREAT_ATTR_TAB_EVT:
        if (param->add_attr_tab.status != ESP_GATT_OK) {
            ESP_LOGE(TAG, "create attr tab failed: %d", param->add_attr_tab.status);
            return;
        }
        if (param->add_attr_tab.num_handle != IDX_NB) {
            ESP_LOGE(TAG, "attr num mismatch: %u != %u",
                     (unsigned)param->add_attr_tab.num_handle, (unsigned)IDX_NB);
            return;
        }
        memcpy(s_handles, param->add_attr_tab.handles, sizeof(s_handles));
        (void)esp_ble_gatts_start_service(s_handles[IDX_SVC]);
        s_started = true;
        ESP_LOGI(TAG, "NUS data service started (rx_h=%u tx_h=%u cmd_rx_h=%u cmd_tx_h=%u)",
                 s_handles[IDX_CHAR_RX_VAL], s_handles[IDX_CHAR_TX_VAL],
                 s_handles[IDX_CHAR_CMDRX_VAL], s_handles[IDX_CHAR_CMDTX_VAL]);
        break;

    case ESP_GATTS_MTU_EVT:
        s_mtu = param->mtu.mtu;
        ESP_LOGI(TAG, "mtu negotiated: %u (payload %u)", s_mtu,
                 (unsigned)((s_mtu > 3) ? (s_mtu - 3) : 20));
        break;

    case ESP_GATTS_CONNECT_EVT:
        s_conn_id = param->connect.conn_id;
        s_connected = true;
        /* 防御:服务注册是启动期不变式(运行期无 teardown),连接即视为可用 */
        s_started = true;
        ESP_LOGI(TAG, "central connected (conn_id=%u)", (unsigned)s_conn_id);
        /* 主动请求"更耐受 WiFi 共存的连接参数"(1.3-24 后续)。
         * 依据:实测本机此前**从不**设置连接参数(完全听 central 安排),而 AP+STA+BLE 共用一颗射频,
         * 一旦 httpd/门户/MQTT 流量突发,central 侧较短的 supervision timeout 会导致链路超时断连
         * —— 表现为"蓝牙反复断/连、设备像消失"。(同期还实测到 httpd 僵死连接 + BLE 连接态把
         * largest_free 压到 5,632、历史 min 到 360B,故这一条只是**纵深防御**,不是唯一根因。)
         * 规范约束:timeout > (1+latency)×max_int×2 ⇒ 6s > 2×50ms ✓。central 可拒绝,
         * 拒绝时保持原参数、无副作用;实际结果由 GAP 的 UPDATE_CONN_PARAMS 事件回显。 */
        {
            esp_ble_conn_update_params_t cp = {0};
            esp_err_t cpe;

            cp.min_int = 0x18;   /* 24 × 1.25ms = 30ms */
            cp.max_int = 0x28;   /* 40 × 1.25ms = 50ms */
            cp.latency = 0;
            cp.timeout = 600;    /* 600 × 10ms = 6s */
            memcpy(cp.bda, param->connect.remote_bda, sizeof(esp_bd_addr_t));
            cpe = esp_ble_gap_update_conn_params(&cp);
            ESP_LOGI(TAG, "request conn params (30-50ms lat=0 tmo=6s) -> %s",
                     esp_err_to_name(cpe));
        }
        break;

    case ESP_GATTS_DISCONNECT_EVT:
        s_connected = false;
        s_congested = false;
        s_conn_id = 0xFFFF;
        s_mtu = 23;
        app_ble_cmd_on_disconnect();   /* 断连作废命令短时 token(连接级鉴权) */
        ESP_LOGI(TAG, "central disconnected (reason=0x%x)", (unsigned)param->disconnect.reason);
        /* 1.3-03 加固:显式恢复广播(实测有断开后未自动恢复、设备不可见的情形)。
         * 注意:此处处于 BT 栈回调上下文,调用只做"请求",不阻塞。 */
        (void)app_bt_restart_advertising();
        break;

    case ESP_GATTS_WRITE_EVT:
        if (s_started && !param->write.is_prep && param->write.len > 0) {
            if (param->write.handle == s_handles[IDX_CHAR_CMDRX_VAL]) {
                /* CMD RX:命令帧 → 行重组(绝不进数据面,1.3-32) */
                app_ble_cmd_on_write(param->write.value, param->write.len);
            } else if (param->write.handle == s_handles[IDX_CHAR_RX_VAL]) {
                /* RX 特征:中心设备写数据 → 只入队(单块长度由 MTU 决定) */
                if (s_write_logged < 6u || (s_write_seen % 20u) == 0u) {
                    s_write_logged++;
                    ESP_LOGI(TAG, "rx write #%u len=%u handle=%u need_rsp=%d", (unsigned)s_write_seen,
                             (unsigned)param->write.len, (unsigned)param->write.handle,
                             (int)param->write.need_rsp);
                }
                s_write_seen++;
                app_ble_dataplane_on_write(param->write.value, param->write.len);
            } else if (s_write_ignored < 6u) {
                s_write_ignored++;
                ESP_LOGW(TAG,
                         "rx write IGNORED: handle=%u (rx_val=%u cmd_rx_val=%u) prep=%d len=%u started=%d connected=%d",
                         (unsigned)param->write.handle, (unsigned)s_handles[IDX_CHAR_RX_VAL],
                         (unsigned)s_handles[IDX_CHAR_CMDRX_VAL],
                         (int)param->write.is_prep, (unsigned)param->write.len,
                         (int)s_started, (int)s_connected);
            }
        } else if (s_write_ignored < 6u) {
            s_write_ignored++;
            ESP_LOGW(TAG,
                     "rx write IGNORED: handle=%u (rx_val=%u cmd_rx_val=%u) prep=%d len=%u started=%d connected=%d",
                     (unsigned)param->write.handle, (unsigned)s_handles[IDX_CHAR_RX_VAL],
                     (unsigned)s_handles[IDX_CHAR_CMDRX_VAL],
                     (int)param->write.is_prep, (unsigned)param->write.len,
                     (int)s_started, (int)s_connected);
        }
        break;

    case ESP_GATTS_CONGEST_EVT:
        /* 发送拥塞:数据面据此暂停/丢弃(背压信号) */
        s_congested = param->congest.congested;
        if (s_congested) {
            ESP_LOGW(TAG, "link congested, pausing data plane output");
        }
        break;

    default:
        break;
    }
}

esp_err_t app_ble_gatts_start(void)
{
    esp_err_t err;

    err = esp_ble_gatts_register_callback(app_ble_gatts_event_handler);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register callback failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gatts_app_register(GATTS_APP_ID);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "app register failed: %s", esp_err_to_name(err));
        return err;
    }
    /* 命令外壳任务(行重组 → 分发 → 回包),幂等 */
    if (app_ble_cmd_start() != ESP_OK) {
        ESP_LOGW(TAG, "ble cmd shell start failed");
    }
    return ESP_OK;
}

void app_ble_gatts_reset(void)
{
    s_connected = false;
    s_congested = false;
    s_conn_id = 0xFFFF;
    s_mtu = 23;
    s_started = false;
}

bool app_ble_gatts_notify(const uint8_t *data, size_t len, void *ctx)
{
    size_t offset = 0;
    uint16_t chunk;

    (void)ctx;
    if (!s_connected || s_congested || !s_started || data == NULL || len == 0) {
        return false;
    }
    chunk = (uint16_t)((s_mtu > 3) ? (s_mtu - 3) : 20);
    while (offset < len) {
        size_t n = len - offset;
        esp_err_t err;

        if (n > chunk) {
            n = chunk;
        }
        /* need_confirm = false → notify(不占往返);拥塞由 CONGEST_EVT 反馈 */
        err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_CHAR_TX_VAL],
                                          (uint16_t)n, (uint8_t *)(data + offset), false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "notify failed: %s", esp_err_to_name(err));
            return offset > 0; /* 已送出前缀则视为部分成功 */
        }
        offset += n;
    }
    return true;
}

/* CMD TX 特征发送(命令回包,1.3-32):与数据 TX 同款 MTU-3 分片,物理上独立特征 */
int app_ble_gatts_cmd_notify(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    uint16_t chunk;

    if (!s_connected || s_congested || !s_started || data == NULL || len == 0) {
        return -1;
    }
    chunk = (uint16_t)((s_mtu > 3) ? (s_mtu - 3) : 20);
    while (offset < len) {
        size_t n = len - offset;
        esp_err_t err;

        if (n > chunk) {
            n = chunk;
        }
        err = esp_ble_gatts_send_indicate(s_gatts_if, s_conn_id, s_handles[IDX_CHAR_CMDTX_VAL],
                                          (uint16_t)n, (uint8_t *)(data + offset), false);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "cmd notify failed: %s", esp_err_to_name(err));
            return (int)offset;
        }
        offset += n;
    }
    return (int)len;
}

bool app_ble_gatts_connected(void)
{
    return s_connected;
}

uint16_t app_ble_gatts_mtu(void)
{
    return s_mtu;
}
#endif /* !CONFIG_BT_NIMBLE_ENABLED */
