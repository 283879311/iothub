#include <stdio.h>
#include <stdarg.h>
#include <string.h>

#include "esp_bt.h"
#if !CONFIG_BT_NIMBLE_ENABLED
#include "esp_bt_defs.h"
#include "esp_bt_main.h"
#include "esp_gap_ble_api.h"
#endif
#include "esp_check.h"
#include "esp_log.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bt.h"
#include "bt_gattc.h"
#include "bt_gatts.h"
#include "common.h"
#include "constants.h"
#include "transport_ble.h"

static const char *TAG = "app_bt";

extern app_config_t s_config;

/* 运行态由自旋锁保护:写入方包括 BT 栈回调线程,读取方包括 Web 线程,
 * 临界区内只有定长拷贝,任意上下文均安全。 */
static uint8_t s_bt_runtime_mode = BT_MODE_OFF;
static char s_bt_last_error[64] = "";
static portMUX_TYPE s_bt_status_mux = portMUX_INITIALIZER_UNLOCKED;

/* 蓝牙协议栈是否真实启动过。mode=off 时 app_bt_release_unused_memory() 已把
 * 整个 _bt_bss 段(含 libbt 的全部状态变量,如 bluedroid 的 s_bt_host_state)
 * 还给堆,此后任何 libbt/controller 的状态查询读到的都是垃圾值,
 * 必须以本标记为准决定是否触碰栈 API,否则会在 OFF 首配时崩溃(见 问题与版本优化.md 第 19 项)。 */
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

/* ---- 广播看门狗(P0-6/1.3-92) --------------------------------------------
 * 背景:三类路径都能进入"无连接、无广播"的静默死亡,且互不重叠:
 *   1) 断连回调里 adv_start 一次失败(EBUSY 竞态)无重试;
 *   2) CONNECT status!=0(host 已停广播,无人重启);
 *   3) ADV_COMPLETE 意外触发后无人重启;
 *   4) s_adv_active 卡 true 使 restart 早退静默 no-op(NimBLE 独有守卫)。
 * 治法:diag 1s 节拍驱动本看门狗,判据只认权威广播态(不认标志),
 * 连续 6s 不满足即重试,退避 2/4/8/16/30s 封顶后永不放弃只降频。
 * 一个机制覆盖全部路径;失败 rc 留在 last_error 供分诊:
 *   rc=22(BLE_HS_ENOTSYNCED) 持续出现 = host 级故障,需复位;
 *   rc=15(BLE_HS_EBUSY) 等 = 广播层竞态,看门狗应能自愈。 */
static const char *s_adv_last_writer = "init";
static uint32_t s_adv_retry_count = 0;

/* 看门狗参数(1.3-92):连续静默阈值与退避封顶 */
#define APP_ADV_WATCHDOG_SILENT_LIMIT_S 5
#define APP_ADV_WATCHDOG_RETRY_CAP_S    30

static void app_bt_adv_note_writer(const char *writer)
{
    s_adv_last_writer = writer;
}

/* 各栈分支提供(定义在 #if/#else 段内):权威广播态 + 看门狗判据实现 */
static bool app_bt_adv_authoritative(void);
static void app_bt_adv_watchdog_tick_impl(uint32_t *silent_ticks, uint32_t *next_retry_s);
static uint8_t app_bt_status_get_mode(void);   /* 定义在本文件下方 */

uint32_t app_bt_adv_retry_count(void)
{
    return s_adv_retry_count;
}

/* 看门狗主体(diag 1s 节拍调用):栈相关的"权威广播态/连接态/sync 态"
 * 由各栈分支的 tick_impl 提供,计数/退避/日志语义单份维护 */
static void app_bt_adv_watchdog_tick(void)
{
    static uint32_t s_silent_ticks = 0;
    static uint32_t s_next_retry_s = 0;

    /* OFF/未启动:看门狗不干预(含 Bluedroid ADV 失败置 OFF 的既有语义) */
    if (!s_stack_started || app_bt_status_get_mode() != BT_MODE_BLE) {
        s_silent_ticks = 0;
        s_next_retry_s = 0;
        return;
    }
    app_bt_adv_watchdog_tick_impl(&s_silent_ticks, &s_next_retry_s);
}

/* 1.3-64:供 NimBLE central 模块在连接建立后清除 "central_scanning";
 * peripheral 侧在广播启动/连接建立/断开后重广播时由 bt.c 自行清空。 */
void app_bt_status_clear_error(void)
{
    app_bt_status_set_error("");
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
/* ============================================================================
 * 1.3-56(NimBLE 迁移 Step 2):NimBLE 生命周期分支 ——
 *   nimble_port_init(内含 controller init/enable + esp_nimble_hci_init)
 *   + nimble_port_freertos_init(host 任务跑 nimble_port_run + ble_hs_sync)
 *   + 全局 GAP 回调(ble_hs_cfg.gap_cb)统一收发连接/订阅/MTU/广播事件,
 *     转发给 bt_gatts_nimble(central 侧 Step 3 再接 bt_gattc_nimble)。
 * ============================================================================ */
/* 广播状态:Bluedroid 由 GAP START/STOP 事件维护,NimBLE 由
 * ADV_COMPLETE/adv_start 返回值维护(两分支共用,1.3-03) */
static bool s_adv_active = false;
/* NimBLE:广播只能在 host sync 之后开启(host 启动流程 ble_hs_start →
 * ble_gatts_start 要求"无活动 GAP 过程",先开广播会触发 assert 崩溃,1.3-56 实测) */
static bool s_adv_requested = false;
/* 1.3-58:host sync 后走 peripheral(GATTS+广播)还是 central(GATTC 扫描) */
static bool s_central_mode = false;

#if CONFIG_BT_NIMBLE_ENABLED

#include "esp_bt.h"

#include "host/ble_gap.h"
#include "host/ble_hs.h"

/* NUS Service UUID 与 bt_gatts_nimble.c 共用同一常量，避免广播与 GATT 不一致。 */
extern const ble_uuid128_t g_ble_nus_svc_uuid;
#include "nimble/nimble_port.h"
#include "nimble/nimble_port_freertos.h"

static struct ble_gap_adv_params s_nimble_adv_params = {
    .conn_mode = BLE_GAP_CONN_MODE_UND,
    .disc_mode = BLE_GAP_DISC_MODE_GEN,
    .itvl_min = 0x20,      /* 32 × 0.625ms = 20ms(与 Bluedroid 侧同参) */
    .itvl_max = 0x40,      /* 40 × 0.625ms = 25ms */
    .channel_map = 0,      /* 三信道 */
};

static int app_bt_nimble_gap_cb(struct ble_gap_event *event, void *arg)
{
    (void)arg;

    switch (event->type) {
    case BLE_GAP_EVENT_ADV_COMPLETE:
        s_adv_active = false;
        app_bt_adv_note_writer("adv_complete");
        break;
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_adv_active = false;   /* 建立连接时广播停止(NimBLE 不自动恢复,1.3-03) */
            app_bt_adv_note_writer("connect_ok");
            app_bt_status_set_error("");   /* 1.3-64:连接已建立,清掉 ble_starting */
            app_ble_gatts_nimble_on_connect(event->connect.conn_handle);
        } else {
            /* P0-6 路径2:连接建立失败(central 中止/握手失败)时 host 已停广播,
             * 此处不重启(时序敏感,重启交给看门狗 ≤6s 兜底),但必须可见 + 清标志 */
            s_adv_active = false;
            app_bt_adv_note_writer("connect_fail");
            ESP_LOGW(TAG, "connect attempt failed: status=%d (adv stopped; "
                     "watchdog will recover)", (int)event->connect.status);
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        app_ble_gatts_nimble_on_disconnect(event->disconnect.reason);
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        app_ble_gatts_nimble_on_subscribe(event->subscribe.conn_handle,
                                          event->subscribe.attr_handle,
                                          event->subscribe.cur_notify);
        break;
    case BLE_GAP_EVENT_MTU:
        app_ble_gatts_nimble_on_mtu(event->mtu.value);
        break;
    case BLE_GAP_EVENT_CONN_UPDATE: {
        struct ble_gap_conn_desc desc = {0};

        if (ble_gap_conn_find(event->conn_update.conn_handle, &desc) == 0) {
            ESP_LOGI(TAG, "conn params: status=%d int=%ums latency=%u timeout=%ums",
                     (int)event->conn_update.status,
                     (unsigned)(desc.conn_itvl * 1250u / 1000u),
                     (unsigned)desc.conn_latency,
                     (unsigned)(desc.supervision_timeout * 10u));
        }
        break;
    }
    default:
        break;
    }
    return 0;
}

static int app_bt_nimble_start_adv(void);

static void app_bt_nimble_on_sync(void)
{
    if (s_central_mode) {
        app_ble_gattc_nimble_on_sync();   /* 主角色:host 就绪后开始扫描 */
        return;
    }
    app_ble_gatts_nimble_on_sync();   /* peripheral 服务注册(host 任务上下文) */
    if (s_adv_requested && !app_bt_adv_authoritative()) {
        (void)app_bt_nimble_start_adv();
    }
}

static void app_bt_nimble_host_task(void *param)
{
    (void)param;
    nimble_port_run();                    /* 阻塞:host 事件循环 */
    nimble_port_freertos_deinit();
}

static esp_err_t app_bt_init_stack(esp_bt_mode_t bt_mode)
{
    esp_err_t err;

    (void)bt_mode;
    /* nimble_port_init 内部:controller init + enable(ESP_BT_MODE_BLE)
     * + esp_nimble_hci_init + host 初始化;只在控制器 IDLE 时调用 */
    /* 注入回调:lib 里 ble_hs_cfg 是强定义(nimble/host/src/ble_hs_cfg.c),
     * 应用只能改写其字段,不能重新定义。 */
    ble_hs_cfg.sync_cb = app_bt_nimble_on_sync;

    if (esp_bt_controller_get_status() == ESP_BT_CONTROLLER_STATUS_IDLE) {
        err = nimble_port_init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "nimble_port_init failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    nimble_port_freertos_init(app_bt_nimble_host_task);
    s_stack_started = true;
    return ESP_OK;
}

static void app_bt_cleanup_stack(void)
{
    esp_bt_controller_status_t controller_status;

    if (!s_stack_started) {
        return;
    }
    s_stack_started = false;

    (void)nimble_port_stop();          /* 停 host 任务(等待退出) */
    (void)nimble_port_deinit();        /* 释放 host 内存 + esp_nimble_hci_deinit */

    controller_status = esp_bt_controller_get_status();
    if (controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (controller_status != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
    }
}

/* 开机一次性:OFF 模式释放 BTDM 全部静态内存(NimBLE 构建同样有效,
 * esp_bt_mem_release 实现在控制器层)。 */
void app_bt_release_unused_memory(void)
{
    static bool s_released = false;
    uint8_t bt_mode;
    uint8_t bt_role;
    size_t free_before;
    size_t free_after;
    esp_err_t err;

    if (s_released) {
        return;
    }
    s_released = true;

    app_config_lock();
    bt_mode = s_config.bt_mode;
    bt_role = s_config.bt_role;
    app_config_unlock();

    /* BLE 常态开启：peripheral 或兼容 BLE 模式都不能释放 BTDM。
     * 旧 off/central 配置已由 app_config_force_ble_peripheral() 收敛。 */
    if (bt_mode == BT_MODE_BLE || bt_role == BT_ROLE_PERIPHERAL) {
        ESP_LOGI(TAG, "BT memory release skipped (mode=%s role=%u, BLE always-on)",
                 app_bt_mode_to_string(bt_mode), (unsigned)bt_role);
        return;
    }

    free_before = (size_t)esp_get_free_heap_size();
    err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
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

static void app_bt_nimble_build_short_name(char *out, size_t out_size)
{
    const char *full = s_config.bt_device_name;
    size_t len = strlen(full);
    const char *suffix = (len >= 4) ? full + len - 4 : full;

    snprintf(out, out_size, "IH-%.4s", suffix);
}

static int app_bt_nimble_start_adv(void)
{
    struct ble_hs_adv_fields adv = {0};
    struct ble_hs_adv_fields rsp = {0};
    char short_name[10];
    size_t full_len;
    int rc;

    app_bt_nimble_build_short_name(short_name, sizeof(short_name));

    /* ADV: flags(3) + UUID128(18) + shortened name(2+7)=30B，留 1B 余量。
     * 不把 tx power 放 ADV；完整名和 tx power 放 Scan Response。 */
    adv.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    adv.uuids128 = &g_ble_nus_svc_uuid;
    adv.num_uuids128 = 1;
    adv.uuids128_is_complete = 1;
    adv.name = (const uint8_t *)short_name;
    adv.name_len = (uint8_t)strlen(short_name);
    adv.name_is_complete = 0;

    rc = ble_gap_adv_set_fields(&adv);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv set fields failed: %d", rc);
        app_bt_status_set_errorf("adv_fields:%d", rc);
        return rc;
    }

    full_len = strlen(s_config.bt_device_name);
    if (full_len > 27) {
        full_len = 27;   /* Scan Response: name(2+N)+tx_power(2) <=31 */
    }
    rsp.name = (const uint8_t *)s_config.bt_device_name;
    rsp.name_len = (uint8_t)full_len;
    rsp.name_is_complete = (full_len == strlen(s_config.bt_device_name)) ? 1 : 0;
    rsp.tx_pwr_lvl_is_present = 1;
    rsp.tx_pwr_lvl = 0;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "scan response set fields failed: %d", rc);
    }

    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &s_nimble_adv_params, app_bt_nimble_gap_cb, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "adv start failed: %d", rc);
        app_bt_status_set_errorf("adv_start:%d", rc);
        return rc;
    }
    s_adv_active = true;
    app_bt_adv_note_writer("start_adv");
    app_bt_status_set_error("");   /* 1.3-64:广播已起来,清掉 ble_starting */
    ESP_LOGI(TAG, "BLE advertising started");
    return 0;
}

static void app_bt_stop(void)
{
    uint8_t runtime_mode = app_bt_status_get_mode();

    app_ble_dataplane_reset();

    if (runtime_mode == BT_MODE_BLE && s_adv_active) {
        (void)ble_gap_adv_stop();
        s_adv_active = false;
        app_bt_adv_note_writer("bt_stop");
    }

    app_bt_cleanup_stack();

    app_bt_status_set_mode(BT_MODE_OFF);
    /* 不在此处清空 last_error:启动失败路径的错误信息靠它带出(问题与版本优化.md 第 23 项) */
}

static esp_err_t app_bt_start_ble(void)
{
    esp_err_t err;

    s_central_mode = false;
    err = app_bt_init_stack(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    if (app_ble_dataplane_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE data plane start failed (continue without data plane)");
    }
    s_adv_requested = true;   /* 广播由 on_sync 在宿主就绪后开启 */
    app_bt_status_set_mode(BT_MODE_BLE);
    app_bt_status_set_error("ble_starting");
    return ESP_OK;
}

static esp_err_t app_bt_start_central(void)
{
    esp_err_t err;

    s_central_mode = true;
    /* 先注册数据面端口 + 初始化 central 状态,再起 host;
     * 扫描在 sync_cb(host 任务) 里启动,避免 sync 前触碰 GAP。 */
    if (app_ble_dataplane_start_central() != ESP_OK) {
        ESP_LOGW(TAG, "BLE central data plane start failed");
    }
    err = app_bt_init_stack(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    app_bt_status_set_mode(BT_ROLE_CENTRAL);
    app_bt_status_set_error("central_scanning");
    return ESP_OK;
}

esp_err_t app_bt_stack_ensure(void)
{
    /* 幂等:控制器 IDLE 才 init,重复调用安全 */
    return app_bt_init_stack(ESP_BT_MODE_BLE);
}

void app_bt_stack_teardown(void)
{
    app_bt_stop();
}

esp_err_t app_bt_restart_advertising(void)
{
    int rc;

    if (s_central_mode) {
        return ESP_ERR_INVALID_STATE;   /* 主角色不广播(1.3-58) */
    }
    /* P0-6 路径4:早退只认权威广播态(ble_gap_adv_active),不认 s_adv_active ——
     * 标志卡 true 时这里曾静默 no-op,设备从此不可见。 */
    if (app_bt_adv_authoritative()) {
        /* 对齐 Bluedroid 语义:已在广播 = 正常;顺手清掉残留的 adv 错误,
         * 消灭"广播活着但 last_error 还挂着"的不一致(如上次 adv_restart:15)。 */
        ESP_LOGI(TAG, "advertising already active (restart skipped, last_writer=%s)",
                 s_adv_last_writer);
        app_bt_status_set_error("");
        return ESP_OK;
    }
    rc = ble_gap_adv_start(BLE_OWN_ADDR_PUBLIC, NULL, BLE_HS_FOREVER,
                           &s_nimble_adv_params, app_bt_nimble_gap_cb, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "advertising restart failed: %d (free=%u)",
                 rc, (unsigned)esp_get_free_heap_size());
        app_bt_status_set_errorf("adv_restart:%d", rc);
        return ESP_FAIL;
    }
    s_adv_active = true;
    app_bt_adv_note_writer("restart");
    app_bt_status_set_error("");   /* 1.3-64:断开后重广播成功,清空状态 */
    ESP_LOGI(TAG, "advertising restart requested (after disconnect)");
    return ESP_OK;
}

/* diag 1s 节拍驱动的广播看门狗入口(1.3-92):跨任务安全 ——
 * NimBLE 的 ble_gap_adv_start 由 host mutex 串行化,本仓库已有
 * cmd 任务调 bt.c 接口的先例(app_bt_apply_config) */
void app_bt_adv_watchdog_poll(void)
{
    app_bt_adv_watchdog_tick();
}

/* NimBLE 的权威广播态:协议栈实时值,非手工标志 */
static bool app_bt_adv_authoritative(void)
{
    return ble_gap_adv_active() != 0;
}

/* 看门狗 tick(diag 1s 节拍调用;主体与栈无关,判据的栈相关项由本分支提供) */
static void app_bt_adv_watchdog_tick_impl(uint32_t *silent_ticks, uint32_t *next_retry_s)
{
    static bool s_desync_reported = false;
    bool connected = app_ble_gatts_connected() || app_ble_gattc_connected();
    bool authoritative = app_bt_adv_authoritative();
    bool flag = s_adv_active;

    /* desync 只诊断不动作:权威 true 时重启只会 EBUSY;方向变化打一次 */
    if (authoritative != flag) {
        if (!s_desync_reported) {
            ESP_LOGW(TAG, "adv flag desync: flag=%d host=%d last_writer=%s "
                     "(treat host as truth)", (int)flag, (int)authoritative,
                     s_adv_last_writer);
            s_desync_reported = true;
        }
    } else {
        s_desync_reported = false;   /* 出口去抖:退出 desync 后允许再次报告 */
    }

    /* 判据:非 central + host 已 sync + 无连接 + 权威广播停。任一不满足即清零。 */
    if (s_central_mode || connected || !ble_hs_synced() || authoritative) {
        *silent_ticks = 0;
        *next_retry_s = 0;
        return;
    }
    (*silent_ticks)++;
    if (*silent_ticks <= APP_ADV_WATCHDOG_SILENT_LIMIT_S) {
        return;
    }
    if (*next_retry_s > 0) {
        (*next_retry_s)--;
        return;
    }
    {
        uint32_t attempt = ++s_adv_retry_count;
        esp_err_t err = app_bt_restart_advertising();

        if (err == ESP_OK) {
            ESP_LOGW(TAG, "adv watchdog: advertising recovered (attempt=%u, last_writer=%s)",
                     (unsigned)attempt, s_adv_last_writer);
            *silent_ticks = 0;
            *next_retry_s = 0;
            return;
        }
        ESP_LOGW(TAG, "adv watchdog retry #%u failed (rc implies host state; "
                 "rc=22 persistent => host dead, reboot device)", (unsigned)attempt);
        *next_retry_s = (*next_retry_s == 0) ? 2
                       : (*next_retry_s < 8) ? (*next_retry_s * 2)
                       : (uint32_t)(APP_ADV_WATCHDOG_RETRY_CAP_S - 2);
        if (*next_retry_s + 2 > APP_ADV_WATCHDOG_RETRY_CAP_S) {
            *next_retry_s = APP_ADV_WATCHDOG_RETRY_CAP_S;
        }
    }
}

bool app_bt_advertising(void)
{
    return s_adv_active;
}

esp_err_t app_bt_apply_config(void)
{
    app_bt_stop();

    /* 产品基线：BLE 始终为 Peripheral。central/off 仅保留代码兼容，
     * 不在运行时生效；旧配置由 config 层收敛并持久化。 */
    app_config_force_ble_peripheral();
    if (s_config.bt_role != BT_ROLE_PERIPHERAL) {
        ESP_LOGE(TAG, "BLE role could not be forced to peripheral (role=%u)",
                 (unsigned)s_config.bt_role);
    }
    return app_bt_start_ble();
}

#else

/* 开机一次性:按配置的蓝牙模式释放“永不使用”的栈内存。
 * OFF 释放 BTDM 全部;BLE 无需释放(BLE 已是构建期唯一保留的栈)。
 * Classic/SPP 自 v1.2 起整体移除(CONFIG_BT_CLASSIC_ENABLED=n),被释放的
 * 模式在本上电周期内不可再启用;任何蓝牙配置修改一律保存后整机重启,不做
 * 运行期热重配(问题与版本优化.md 第 21 项)。 */
void app_bt_release_unused_memory(void)
{
    static bool s_released = false;
    uint8_t bt_mode;
    uint8_t bt_role;
    size_t free_before;
    size_t free_after;
    esp_err_t err;

    if (s_released) {
        return;
    }
    s_released = true;

    app_config_lock();
    bt_mode = s_config.bt_mode;
    bt_role = s_config.bt_role;
    app_config_unlock();

    /* BLE 常态开启：peripheral 或兼容 BLE 模式都不能释放 BTDM。
     * 旧 off/central 配置已由 app_config_force_ble_peripheral() 收敛。 */
    if (bt_mode == BT_MODE_BLE || bt_role == BT_ROLE_PERIPHERAL) {
        ESP_LOGI(TAG, "BT memory release skipped (mode=%s role=%u, BLE always-on)",
                 app_bt_mode_to_string(bt_mode), (unsigned)bt_role);
        return;
    }

    free_before = (size_t)esp_get_free_heap_size();
    /* OFF(或历史遗留值)统一释放整个 BT 栈内存;esp_bt_mem_release 只有传
     * ESP_BT_MODE_BTDM 才会真正回收 BSS/Data 段(BLE_ONLY 构建下同样适用) */
    err = esp_bt_mem_release(ESP_BT_MODE_BTDM);
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

static void app_ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_ble_adv_configured = true;
        esp_ble_gap_start_advertising(&s_ble_adv_params);
        break;
    /* 连接参数更新结果回显(1.3-24 后续):`bt_gatts` 在连接时主动请求 30-50ms / timeout 6s 的参数,
     * 这里打印**实际生效值** —— 用于判定 central 是否接受了请求(拒绝时会回显手机原来的参数)。 */
    case ESP_GAP_BLE_UPDATE_CONN_PARAMS_EVT:
        ESP_LOGI(TAG, "conn params: status=%d int=%ums latency=%u timeout=%ums",
                 (int)param->update_conn_params.status,
                 (unsigned)param->update_conn_params.conn_int,
                 (unsigned)param->update_conn_params.latency,
                 (unsigned)param->update_conn_params.timeout * 10u);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        s_adv_active = (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS);
        app_bt_adv_note_writer(s_adv_active ? "adv_start_ok" : "adv_start_fail");
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

static esp_err_t app_bt_init_stack(esp_bt_mode_t bt_mode)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_bt_controller_status_t ctrl_status;
    esp_bluedroid_status_t bluedroid_status;
    esp_err_t err;

    /* esp_bt_controller_enable(mode) 要求 mode 与 init 时的 cfg->mode 一致
     * (IDF bt.c 历史约束);BTDM 构建的默认模板是 ESP_BT_MODE_BTDM,
     * 不按目标模式覆写则 enable 恒返回 ESP_ERR_INVALID_ARG(问题与版本优化.md 第 23 项) */
    bt_cfg.mode = bt_mode;

    /* Bluedroid 把连接完成/嗅探切换等正常生命周期事件以 W 级打印,压到 ERROR
     * 只留真实异常;应用层事件由 app_bt 自身的 INFO 日志覆盖 */
    esp_log_level_set("BT_HCI", ESP_LOG_ERROR);
    esp_log_level_set("BT_APPL", ESP_LOG_ERROR);

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

esp_err_t app_bt_restart_advertising(void)
{
    esp_err_t err;

    if (!s_ble_adv_configured) {
        return ESP_ERR_INVALID_STATE;   /* 广播数据未配置(非 BLE 模式):无需处理 */
    }
    err = esp_ble_gap_start_advertising(&s_ble_adv_params);
    if (err == ESP_ERR_INVALID_STATE) {
        /* 已经在广播(栈可能已自动恢复)→ 视为正常 */
        ESP_LOGI(TAG, "advertising already active");
        app_bt_status_set_error("");
        return ESP_OK;
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "advertising restart failed: %s (free=%u)",
                 esp_err_to_name(err), (unsigned)esp_get_free_heap_size());
        return err;
    }
    app_bt_adv_note_writer("restart");
    ESP_LOGI(TAG, "advertising restart requested (after disconnect)");
    return ESP_OK;
}

bool app_bt_advertising(void)
{
    return s_adv_active;
}

/* Bluedroid:BTA 不暴露 adv-active 权威查询,标志就是唯一事实源(1.3-92);
 * restart 的 INVALID_STATE 容忍已兜住"标志错报已停"的情形 */
static bool app_bt_adv_authoritative(void)
{
    return s_adv_active;
}

/* Bluedroid 看门狗 tick_impl:无 desync 检测(权威=标志,永不 desync)、
 * 无 sync 判断(BTA 自带状态机) */
static void app_bt_adv_watchdog_tick_impl(uint32_t *silent_ticks, uint32_t *next_retry_s)
{
    bool connected = app_ble_gatts_connected() || app_ble_gattc_connected();

    if (connected || app_bt_adv_authoritative()) {
        *silent_ticks = 0;
        *next_retry_s = 0;
        return;
    }
    (*silent_ticks)++;
    if (*silent_ticks <= APP_ADV_WATCHDOG_SILENT_LIMIT_S) {
        return;
    }
    if (*next_retry_s > 0) {
        (*next_retry_s)--;
        return;
    }
    {
        uint32_t attempt = ++s_adv_retry_count;
        esp_err_t err = app_bt_restart_advertising();

        if (err == ESP_OK) {
            ESP_LOGW(TAG, "adv watchdog: advertising recovered (attempt=%u, last_writer=%s)",
                     (unsigned)attempt, s_adv_last_writer);
            *silent_ticks = 0;
            *next_retry_s = 0;
            return;
        }
        ESP_LOGW(TAG, "adv watchdog retry #%u failed: %s",
                 (unsigned)attempt, esp_err_to_name(err));
        *next_retry_s = (*next_retry_s == 0) ? 2
                       : (*next_retry_s < 8) ? (*next_retry_s * 2)
                       : (uint32_t)(APP_ADV_WATCHDOG_RETRY_CAP_S - 2);
        if (*next_retry_s + 2 > APP_ADV_WATCHDOG_RETRY_CAP_S) {
            *next_retry_s = APP_ADV_WATCHDOG_RETRY_CAP_S;
        }
    }
}

static void app_bt_stop(void)
{
    uint8_t runtime_mode = app_bt_status_get_mode();

    app_ble_dataplane_reset();

    if (runtime_mode == BT_MODE_BLE && s_ble_adv_configured) {
        esp_ble_gap_stop_advertising();
        s_ble_adv_configured = false;
    }

    app_bt_cleanup_stack();

    app_bt_status_set_mode(BT_MODE_OFF);
    /* 不在此处清空 last_error:启动失败路径的错误信息靠它带出(问题与版本优化.md 第 23 项) */
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
    /* BLE 数据面 GATT 服务(NUS 兼容 UUID;与配网特征不同 UUID,可同栈共存) */
    if (app_ble_dataplane_start() != ESP_OK) {
        ESP_LOGW(TAG, "BLE data plane start failed (continue without data plane)");
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

static esp_err_t app_bt_start_central(void)
{
    esp_err_t err;

    err = app_bt_init_stack(ESP_BT_MODE_BLE);
    if (err != ESP_OK) {
        app_bt_status_set_errorf("bt_stack:%s", esp_err_to_name(err));
        app_bt_stop();
        return err;
    }
    /* 主角色:数据面出站 = 写对端 RX 特征;GAP 扫描回调由 bt_gattc 独占
     * (本模式下 bt.c 不注册自有 GAP 回调、不广播) */
    if (app_ble_dataplane_start_central() != ESP_OK) {
        ESP_LOGW(TAG, "BLE central data plane start failed");
    }
    s_stack_started = true;
    app_bt_status_set_mode(BT_ROLE_CENTRAL);
    app_bt_status_set_error("central_scanning");
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * 栈底座:供配网 / 数据面复用的唯一触碰蓝牙栈的入口
 * ------------------------------------------------------------------------- */
esp_err_t app_bt_stack_ensure(void)
{
    /* 幂等:app_bt_init_stack 内部按控制器/主机状态判断,重复调用安全 */
    return app_bt_init_stack(ESP_BT_MODE_BLE);
}

void app_bt_stack_teardown(void)
{
    app_bt_stop();
}

esp_err_t app_bt_apply_config(void)
{
    app_bt_stop();

    /* 产品基线：BLE 始终为 Peripheral。central/off 仅保留代码兼容，
     * 不在运行时生效；旧配置由 config 层收敛并持久化。 */
    app_config_force_ble_peripheral();
    if (s_config.bt_role != BT_ROLE_PERIPHERAL) {
        ESP_LOGE(TAG, "BLE role could not be forced to peripheral (role=%u)",
                 (unsigned)s_config.bt_role);
    }
    return app_bt_start_ble();
}
#endif /* CONFIG_BT_NIMBLE_ENABLED */
