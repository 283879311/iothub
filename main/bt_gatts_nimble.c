/* ============================================================================
 * bt_gatts_nimble.c — BLE 数据面 GATT 服务(peripheral 侧)的 NimBLE 实现
 *
 * 1.3-56(NimBLE 迁移 Step 2):与 bt_gatts.c(Bluedroid 实现)同 facade、
 * 同 NUS 服务形态(4 特征值,RX/TX/CMD RX/CMD TX),全部 NimBLE API。
 * 差异要点(见 问题与版本优化.md 附录 A A.2):
 *   - 服务表:ble_gatt_svc_def(CCCD 由栈自动生成,不手写)
 *   - 写回调:每特征 access_cb(BLE_GATT_ACCESS_OP_WRITE_CHR,数据在 os_mbuf)
 *   - notify:ble_gatts_notify_custom + os_msys_get_pkthdr(分片按 ble_att_mtu-3)
 *   - 连接/订阅/MTU:统一 GAP 回调,由 bt.c 转发到本模块
 *   - 服务注册必须在 host 任务 on_sync 里做(ble_gatts_add_svcs 依赖宿主已同步)
 * 硬约束不变:回调内只 memcpy+入队,发送在 data_pipe 任务里完成。
 * ============================================================================ */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include "esp_log.h"   /* 先包含:sdkconfig.h 经此可见,后续 #if CONFIG_* 才有效 */
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#if CONFIG_BT_NIMBLE_ENABLED

#include "host/ble_hs.h"
#include "host/ble_gatt.h"
#include "os/os_mbuf.h"

#include "ble_cmd.h"
#include "bt.h"
#include "bt_gatts.h"
#include "data_pipe.h"
#include "transport_ble.h"

static const char *TAG = "bt_gatts";

/* Nordic UART Service(UUID 小端字节序,与 bt_gatts.c 同一组常量) */
static const ble_uuid128_t nus_svc_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t nus_rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t nus_tx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);
static const ble_uuid128_t nus_cmd_rx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x04, 0x00, 0x40, 0x6E);
static const ble_uuid128_t nus_cmd_tx_uuid = BLE_UUID128_INIT(
    0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0,
    0x93, 0xF3, 0xA3, 0xB5, 0x05, 0x00, 0x40, 0x6E);

/* 单次写入落地容量(必须 ≥ DATA_PIPE_CHUNK_MAX,与 Bluedroid 侧同一不变量) */
#define NUS_RX_VALUE_MAX 256
_Static_assert(NUS_RX_VALUE_MAX >= DATA_PIPE_CHUNK_MAX,
               "RX 属性值容量必须 ≥ 数据面单块上限,否则大帧在 GATT 写层被拒");

/* notify 分片防御上限:msys 池 24×256B,单包超过 512B 时强制降片 */
#define NUS_NOTIFY_CHUNK_CAP 512

static uint16_t s_rx_val_handle;
static uint16_t s_tx_val_handle;
static uint16_t s_cmd_rx_val_handle;
static uint16_t s_cmd_tx_val_handle;
static uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_mtu = 23;
static bool s_connected = false;
static bool s_data_subscribed = false;
static bool s_cmd_subscribed = false;
static bool s_cmd_rx_unsub_warned = false;
static bool s_reg_pending = false;
static bool s_host_synced = false;

/* ---------------------------------------------------------------------------
 * access_cb:只做 memcpy + 入队(命令帧走行重组器,数据帧走 data_pipe)
 * ------------------------------------------------------------------------- */
static int nus_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg);
static int nus_cmd_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg);

static int nus_write_common(struct ble_gatt_access_ctxt *ctxt, bool is_cmd)
{
    uint8_t buf[NUS_RX_VALUE_MAX];
    uint16_t len;
    int rc;

    if (ctxt->op != BLE_GATT_ACCESS_OP_WRITE_CHR) {
        return 0;   /* 只处理写;读请求返回长度 0(与 Bluedroid 侧行为一致) */
    }
    len = OS_MBUF_PKTLEN(ctxt->om);
    if (len == 0) {
        return 0;
    }
    if (len > sizeof(buf)) {
        ESP_LOGW(TAG, "write too long: %u > %u", (unsigned)len, (unsigned)sizeof(buf));
        return BLE_ATT_ERR_UNLIKELY;
    }
    rc = os_mbuf_copydata(ctxt->om, 0, len, buf);
    if (rc != 0) {
        ESP_LOGW(TAG, "mbuf copy failed: %d", rc);
        return BLE_ATT_ERR_UNLIKELY;
    }
    if (is_cmd) {
        ESP_LOGI(TAG, "cmd RX write: %u byte(s) last=0x%02X newline=%d",
                 (unsigned)len, (unsigned)buf[len - 1], buf[len - 1] == '\n' ? 1 : 0);
        if (!s_cmd_subscribed && !s_cmd_rx_unsub_warned) {
            s_cmd_rx_unsub_warned = true;
            ESP_LOGW(TAG,
                     "CMD RX written but CMD TX (6E400005) not subscribed; "
                     "enable notifications on CMD TX to receive replies");
        }
        app_ble_cmd_on_write(buf, len);          /* 命令帧 → 行重组器 */
    } else {
        app_ble_dataplane_on_write(buf, len);    /* 数据帧 → data_pipe(只入队) */
    }
    return 0;
}

static int nus_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    return nus_write_common(ctxt, false);
}

static int nus_cmd_rx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                                struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)arg;
    return nus_write_common(ctxt, true);
}

/* ---------------------------------------------------------------------------
 * 服务表(CCCD 由 NimBLE 自动生成;句柄由栈在 add_svcs 后回填 val_handle)
 * ------------------------------------------------------------------------- */
/* 本 fork 的 ble_gatts_chr_is_sane 要求每个特征都有 access_cb(notify-only 也要):
 * 给个空实现(读返回空、无写权限),与 Bluedroid 侧"TX 读返回 0 字节"行为一致 */
static int nus_tx_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                            struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void)conn_handle;
    (void)attr_handle;
    (void)ctxt;
    (void)arg;
    return 0;
}

static const struct ble_gatt_chr_def nus_chars[] = {
    {
        .uuid = &nus_rx_uuid.u,
        .access_cb = nus_rx_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_rx_val_handle,
    },
    {
        .uuid = &nus_tx_uuid.u,
        .access_cb = nus_tx_access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_tx_val_handle,
    },
    {
        .uuid = &nus_cmd_rx_uuid.u,
        .access_cb = nus_cmd_rx_access_cb,
        .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
        .val_handle = &s_cmd_rx_val_handle,
    },
    {
        .uuid = &nus_cmd_tx_uuid.u,
        .access_cb = nus_tx_access_cb,
        .flags = BLE_GATT_CHR_F_NOTIFY,
        .val_handle = &s_cmd_tx_val_handle,
    },
    { 0 }
};

/* 注意:NimBLE 的 svc_def 参数是"NULL 结尾的数组",单个服务也必须带 {0} 结尾,
 * 否则迭代器越过结构体读到相邻内存(实测:gatts_start ENOMEM、句柄全 0) */
static const struct ble_gatt_svc_def nus_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &nus_svc_uuid.u,
        .characteristics = nus_chars,
    },
    { 0 }
};

/* ---------------------------------------------------------------------------
 * GAP 事件转发入口(bt.c 的 NimBLE GAP 回调调用)
 * ------------------------------------------------------------------------- */
void app_ble_gatts_nimble_on_sync(void)
{
    int rc;

    ESP_LOGI(TAG, "on_sync from task %s", pcTaskGetName(NULL));
    s_host_synced = true;
    if (!s_reg_pending) {
        return;
    }
    rc = ble_gatts_count_cfg(nus_svcs);
    if (rc < 0) {
        ESP_LOGE(TAG, "count_cfg failed: %d", rc);
        return;
    }
    rc = ble_gatts_add_svcs(nus_svcs);
    if (rc != 0) {
        ESP_LOGE(TAG, "add_svcs failed: %d", rc);
        return;
    }
    /* 诊断(1.3-56):gatts_start 失败仍打印句柄 —— 判断 add_svcs 是否已把
     * 服务注册进 ATT 表(rc=ENOMEM 时句柄非零说明注册已生效) */
    ESP_LOGI(TAG, "svc defs staged (rx_h=%u tx_h=%u cmd_rx_h=%u cmd_tx_h=%u)",
             s_rx_val_handle, s_tx_val_handle, s_cmd_rx_val_handle, s_cmd_tx_val_handle);
    rc = ble_gatts_start();
    if (rc != 0) {
        ESP_LOGW(TAG, "gatts_start failed: %d (handles: rx=%u tx=%u cmd_rx=%u cmd_tx=%u)",
                 rc, s_rx_val_handle, s_tx_val_handle, s_cmd_rx_val_handle, s_cmd_tx_val_handle);
    }
    s_reg_pending = false;
    /* 与数据面 256B 单块对齐的 MTU(msys 池友好;central 可请求更高,以协商为准) */
    (void)ble_att_set_preferred_mtu(256);
    ESP_LOGI(TAG, "NUS data service started (nimble, rx_h=%u tx_h=%u cmd_rx_h=%u cmd_tx_h=%u)",
             s_rx_val_handle, s_tx_val_handle, s_cmd_rx_val_handle, s_cmd_tx_val_handle);
}

void app_ble_gatts_nimble_on_connect(uint16_t conn_handle)
{
    struct ble_gap_upd_params cp = {0};
    struct ble_gap_conn_desc desc;

    s_conn_handle = conn_handle;
    s_connected = true;
    s_data_subscribed = false;
    s_cmd_subscribed = false;
    s_cmd_rx_unsub_warned = false;
    s_mtu = 23;
    /* 1.3-74:记录占用连接的 central 地址,便于定位"蓝牙被谁连着导致不广播"。
     * peer_ota 是空口地址(手机隐私地址常见 random),peer_id 是身份地址;
     * 地址按人类可读顺序(MSB→LSB)打印。 */
    if (ble_gap_conn_find(conn_handle, &desc) == 0) {
        ESP_LOGI(TAG,
                 "central connected (conn=%u) peer_ota=%02X:%02X:%02X:%02X:%02X:%02X type=%u "
                 "peer_id=%02X:%02X:%02X:%02X:%02X:%02X type=%u",
                 (unsigned)conn_handle,
                 desc.peer_ota_addr.val[5], desc.peer_ota_addr.val[4],
                 desc.peer_ota_addr.val[3], desc.peer_ota_addr.val[2],
                 desc.peer_ota_addr.val[1], desc.peer_ota_addr.val[0],
                 (unsigned)desc.peer_ota_addr.type,
                 desc.peer_id_addr.val[5], desc.peer_id_addr.val[4],
                 desc.peer_id_addr.val[3], desc.peer_id_addr.val[2],
                 desc.peer_id_addr.val[1], desc.peer_id_addr.val[0],
                 (unsigned)desc.peer_id_addr.type);
    } else {
        ESP_LOGI(TAG, "central connected (conn_handle=%u) peer=unknown", (unsigned)conn_handle);
    }
    /* 主动请求更耐受 WiFi 共存的连接参数(与 Bluedroid 侧 1.3-24 同参):
     * 30-50ms / latency 0 / timeout 6s;central 可拒绝,无副作用 */
    cp.itvl_min = 24;                /* 24 × 1.25ms = 30ms */
    cp.itvl_max = 40;                /* 40 × 1.25ms = 50ms */
    cp.latency = 0;
    cp.supervision_timeout = 600;    /* 600 × 10ms = 6s */
    cp.min_ce_len = 0;
    cp.max_ce_len = 0;
    (void)ble_gap_update_params(conn_handle, &cp);
}

void app_ble_gatts_nimble_on_disconnect(int reason)
{
    ESP_LOGW(TAG, "central disconnected (reason=0x%x)", (unsigned)reason);
    s_connected = false;
    s_data_subscribed = false;
    s_cmd_subscribed = false;
    s_cmd_rx_unsub_warned = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = 23;
    app_ble_cmd_on_disconnect();   /* 断连作废命令短时 token(连接级鉴权) */
    /* 1.3-03 加固:断开后显式恢复广播(NimBLE 同样不会自动恢复) */
    (void)app_bt_restart_advertising();
}

void app_ble_gatts_nimble_on_subscribe(uint16_t conn_handle, uint16_t attr_handle,
                                       uint8_t cur_notify)
{
    bool on = (cur_notify != 0);

    (void)conn_handle;
    if (attr_handle == s_tx_val_handle) {
        s_data_subscribed = on;
        ESP_LOGI(TAG, "data TX subscribe %s (handle=%u)", on ? "on" : "off",
                 (unsigned)attr_handle);
    } else if (attr_handle == s_cmd_tx_val_handle) {
        s_cmd_subscribed = on;
        if (on) {
            s_cmd_rx_unsub_warned = false;
        }
        ESP_LOGI(TAG, "cmd TX subscribe %s (handle=%u)", on ? "on" : "off",
                 (unsigned)attr_handle);
    } else {
        ESP_LOGI(TAG, "subscribe event on other handle=%u notify=%u",
                 (unsigned)attr_handle, (unsigned)cur_notify);
    }
}

void app_ble_gatts_nimble_on_mtu(uint16_t mtu)
{
    s_mtu = mtu;
    ESP_LOGI(TAG, "mtu negotiated: %u (payload %u)", (unsigned)mtu,
             (unsigned)((mtu > 3) ? (mtu - 3) : 20));
}

/* ---------------------------------------------------------------------------
 * facade
 * ------------------------------------------------------------------------- */
esp_err_t app_ble_gatts_start(void)
{
    /* 注册动作延迟到 host 任务 on_sync 执行(ble_gatts_add_svcs 依赖宿主已同步);
     * 若宿主已同步(角色重启路径),这里直接注册 */
    if (!s_reg_pending && s_rx_val_handle == 0) {
        s_reg_pending = true;
        if (s_host_synced) {
            app_ble_gatts_nimble_on_sync();
        }
    }
    if (app_ble_cmd_start() != ESP_OK) {
        ESP_LOGW(TAG, "ble cmd shell start failed");
    }
    return ESP_OK;
}

void app_ble_gatts_reset(void)
{
    s_connected = false;
    s_data_subscribed = false;
    s_cmd_subscribed = false;
    s_cmd_rx_unsub_warned = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
    s_mtu = 23;
    s_reg_pending = false;
}

static bool app_ble_gatts_notify_chunk(uint16_t val_handle, const uint8_t *data, size_t len)
{
    struct os_mbuf *om;
    int rc;

    if (!s_connected || val_handle == 0) {
        return false;
    }
    om = os_msys_get_pkthdr((uint16_t)len, 0);
    if (om == NULL) {
        ESP_LOGW(TAG, "msys out of mbufs (len=%u)", (unsigned)len);
        return false;
    }
    rc = os_mbuf_append(om, data, (uint16_t)len);
    if (rc != 0) {
        os_mbuf_free_chain(om);
        return false;
    }
    rc = ble_gatts_notify_custom(s_conn_handle, val_handle, om);
    if (rc != 0) {
        ESP_LOGW(TAG, "notify failed: rc=0x%x", (unsigned)rc);
        return false;
    }
    return true;
}

bool app_ble_gatts_notify(const uint8_t *data, size_t len, void *ctx)
{
    size_t offset = 0;
    uint16_t chunk;

    (void)ctx;
    if (!s_connected || !s_data_subscribed || s_tx_val_handle == 0 ||
        data == NULL || len == 0) {
        return false;
    }
    chunk = (s_mtu > 3) ? (uint16_t)(s_mtu - 3) : 20;
    if (chunk > NUS_NOTIFY_CHUNK_CAP) {
        chunk = NUS_NOTIFY_CHUNK_CAP;
    }
    while (offset < len) {
        size_t n = len - offset;

        if (n > chunk) {
            n = chunk;
        }
        if (!app_ble_gatts_notify_chunk(s_tx_val_handle, data + offset, n)) {
            return offset > 0;   /* 已送出前缀则视为部分成功 */
        }
        offset += n;
    }
    return true;
}

bool app_ble_gatts_cmd_notify(const uint8_t *data, size_t len)
{
    size_t offset = 0;
    uint16_t chunk;

    if (!s_connected || !s_cmd_subscribed || s_cmd_tx_val_handle == 0 ||
        data == NULL || len == 0) {
        return false;
    }
    chunk = (s_mtu > 3) ? (uint16_t)(s_mtu - 3) : 20;
    if (chunk > NUS_NOTIFY_CHUNK_CAP) {
        chunk = NUS_NOTIFY_CHUNK_CAP;
    }
    while (offset < len) {
        size_t n = len - offset;

        if (n > chunk) {
            n = chunk;
        }
        if (!app_ble_gatts_notify_chunk(s_cmd_tx_val_handle, data + offset, n)) {
            return offset > 0;
        }
        offset += n;
    }
    return true;
}

bool app_ble_gatts_connected(void)
{
    return s_connected;
}

uint16_t app_ble_gatts_mtu(void)
{
    return s_mtu;
}

#endif /* CONFIG_BT_NIMBLE_ENABLED */
