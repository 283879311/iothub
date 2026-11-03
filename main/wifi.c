#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#ifdef APP_WIFI_FAULT_INJECT
#include "esp_attr.h"
#include "esp_rom_crc.h"
#endif
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"

#include "common.h"
#include "config.h"
#include "identity.h"
#include "bt_gatts.h"
#include "wifi.h"

static const char *TAG = "wifi";
/* 同一 profile 断线后先直连重试的次数,超过才拉黑换下一个 profile。
 * 取 3 避免边缘 AP 一次抖动就永久跳走;总时长仍受 90s 启动窗口约束。 */
static const uint8_t APP_WIFI_STA_RETRY_LIMIT = 3;
static const int64_t APP_WIFI_STA_STARTUP_TIMEOUT_MS = 90000;
static const int64_t APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS = 1000;
/* 运行时断网兜底(死点修复,2026-10-27):启动窗口外断网后,原实现只由断连事件驱动
 * —— 直连重试 3 次 + attempted_mask 把已存 profile 各试一次;mask 试满的那轮
 * "no untried saved AP left" 不发起连接,也就不再产生断连事件,链条永久终止,
 * AP 恢复后无人再去扫。本监视器在启动窗口外周期重试:每轮先 fast path 试正式
 * SSID 再扫描选最优未试 profile(mask 每轮清零),节拍 5/10/30/60s 封顶 ——
 * 60s 档每轮 2-3s scan 的占空比约 4%,对 BLE 通知时序无感。 */
static const int64_t APP_WIFI_RUNTIME_RETRY_BACKOFF_MS[] = {5000, 10000, 30000, 60000};
#define APP_WIFI_RUNTIME_RETRY_STEPS \
    (sizeof(APP_WIFI_RUNTIME_RETRY_BACKOFF_MS) / sizeof(APP_WIFI_RUNTIME_RETRY_BACKOFF_MS[0]))
/* BLE 连接期间的降级重试(2026-11-01 产品决策 A):只试正式 SSID,不扫描
 * profile;120s 一次。目的:解决"配置完忘了关小程序/蓝牙,AP 恢复后设备
 * 永久离线"——原门控把"连接存在"当成"用户在场",但链路空闲不代表用户
 * 还在操作。此路径不推进主退避档;BLE 断开后回到完整 5/10/30/60 周期。 */
static const int64_t APP_WIFI_BLE_FAST_RETRY_MS = 120000;
/* D1 看门狗:同步 esp_wifi_connect() 失败/扫描失败可能留下
 * s_wifi_sta_connecting=true 且没有后续断连事件;monitor 两个分支都要求
 * 该标志为 false,一次踩中会永久停止调度。30s 远大于正常关联(实测最慢
 * ~15s),超时强制清零并立即重排。 */
static const int64_t APP_WIFI_CONNECT_STUCK_TIMEOUT_MS = 30000;
/* M1 离线 quiesce(设计稿 §3.6 R3.1/§10.2-7):总等待上限由调用方传入(默认 1000ms),
 * 200ms 告警阈值为内部常量 —— 超限绝不执行 esp_wifi_stop(),宁可暂时
 * 不停止 Wi-Fi,也不让 stop 与 connect 并发。 */
#define APP_WIFI_QUIESCE_WARN_MS 200
static const int64_t APP_WIFI_QUIESCE_LIMIT_DEFAULT_MS = 1000;
static int64_t s_wifi_connecting_since_ms = 0;
static int64_t s_wifi_ble_fast_retry_at_ms = 0; /* 0=尚未排定 */
static uint32_t s_wifi_ble_fast_cycle = 0;      /* 日志关联 */
static bool s_wifi_ble_was_connected = false;   /* 连接态跳变时重置计时 */
/* BLE 连接期间跳轮:用户在场走 BLE 救援/配网,后台扫描只会争射频;
 * 跳轮不推进退避档位,断开后从暂停档继续(不重置回 5s)。 */
static int64_t s_wifi_rt_retry_at_ms = 0;   /* 下次允许发起的时刻;0=立即 */
static uint8_t s_wifi_rt_retry_step = 0;    /* 当前退避档(0..STEPS-1,封顶后不再升) */
static uint32_t s_wifi_rt_retry_cycle = 0;  /* 已发起轮次(日志关联用),拿到 IP 清零 */
/* 只在启动窗口关闭后启用:监视器首 tick 可能早于窗口开启,不设门会在启动期抢跑。
 * 窗口关闭(拿到 IP 或启动失败)即置位,此后设备生命周期内常驻。 */
static bool s_wifi_rt_armed = false;
/* STA 关联的最低可接受安全模式(1.3-04 实测):原实现按"口令 >= 8 位"设 WIFI_AUTH_WPA2_PSK,
 * 会拒连安全模式判定低于 WPA2 的上游(WPA-only / 部分混模路由器),报 reason=210
 * (NO_AP_FOUND_W_COMPATIBLE_SECURITY:找到了 AP 但安全模式与阈值不兼容)。
 * 阈值只作"最低可接受"用,WPA_PSK 已足以挡住开放/弱加密 AP;量产若需收紧改此一处即可。 */
#define APP_WIFI_STA_AUTHMODE_MIN WIFI_AUTH_WPA_PSK

/* ===== A7/A8 连接许可与操作所有权原语(定义集中在文件头,供早期函数使用) ===== */
/* A7:危险段(esp_wifi_stop/配置写入)连接许可封锁,begin 拒绝条件含本标志。
 * A8:释放点改为 STA_START 处理器(apply_cfg 只在危险段前置位,事件里清)。 */
static bool s_wifi_apply_hold = false;

/* A8:Wi-Fi 操作所有权 —— 与 tryconnect_begin 同一 spinlock(s_tryconnect_mux)
 * 内 claim/release,保证 apply 与 tryconnect 事务级互斥。 */
typedef enum {
    APP_WIFI_OP_NONE = 0,
    APP_WIFI_OP_APPLY,
    APP_WIFI_OP_TRYX,
} app_wifi_op_owner_t;
static app_wifi_op_owner_t s_wifi_op_owner = APP_WIFI_OP_NONE;

/* A8:apply 等待自己发起的 STA_START —— 事件处理器在置位期间负责释放
 * 门控/hold、开启动窗口,并置位事件位;apply 侧有界等待,超时按失败处理。 */
static bool s_wifi_apply_await_start = false;
static uint32_t s_wifi_apply_start_gen = 0;
/* A8-修正2:两阶段完成状态 ——
 * consumed_gen: 处理器已消费本次 await(事件已到);
 * completed_gen: 启动窗口+首次调度已完成(处理器尾部);
 * apply 只有在 completed_gen 匹配时才把本次 start 视为完成。 */
static uint32_t s_wifi_apply_start_consumed_gen = 0;
static uint32_t s_wifi_apply_start_completed_gen = 0;
#define APP_WIFI_START_EVENT_BIT BIT0
#define APP_WIFI_START_WAIT_MS 2000
static EventGroupHandle_t s_wifi_start_events = NULL;
static StaticEventGroup_t s_wifi_start_events_buf;

#ifdef APP_WIFI_FAULT_INJECT
typedef struct {
    bool active;
    app_wifi_apply_stage_t fail_stage;
    int fail_count;
    int stop_fail_count;
    bool hold_armed;
    bool hold_release;
    bool hold_claimed;
    uint32_t hold_gen;
    int hold_timeout_ms;
    uint32_t hold_seq;
} app_wifi_test_hook_t;
static app_wifi_test_hook_t s_wifi_hook;
static portMUX_TYPE s_wifi_hook_mux = portMUX_INITIALIZER_UNLOCKED;

/* ---- T5/T6 启动期一次性票据(RTC_NOINIT,不落 NVS) ---- */
#define APP_WIFI_BOOT_TICKET_MAGIC 0x4D314246u   /* 'M1BF' */
#define APP_WIFI_BOOT_TICKET_VERSION 1u
#define APP_WIFI_BOOT_TICKET_PROBE_STAGE 0xFFFFu

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t stage;
    uint8_t  count;
    uint8_t  reserved[3];
    uint32_t crc;
} app_wifi_boot_ticket_t;

RTC_NOINIT_ATTR static app_wifi_boot_ticket_t s_wifi_boot_ticket;
static const char *app_wifi_apply_stage_name(app_wifi_apply_stage_t stage);

static uint32_t app_wifi_boot_ticket_crc(const app_wifi_boot_ticket_t *t)
{
    uint8_t buf[12];
    buf[0] = (uint8_t)(t->magic & 0xff);
    buf[1] = (uint8_t)((t->magic >> 8) & 0xff);
    buf[2] = (uint8_t)((t->magic >> 16) & 0xff);
    buf[3] = (uint8_t)((t->magic >> 24) & 0xff);
    buf[4] = (uint8_t)(t->version & 0xff);
    buf[5] = (uint8_t)((t->version >> 8) & 0xff);
    buf[6] = (uint8_t)(t->stage & 0xff);
    buf[7] = (uint8_t)((t->stage >> 8) & 0xff);
    buf[8] = t->count;
    buf[9] = t->reserved[0];
    buf[10] = t->reserved[1];
    buf[11] = t->reserved[2];
    return esp_rom_crc32_le(0xffffffffu, buf, sizeof(buf));
}

static void app_wifi_boot_ticket_clear(void)
{
    memset(&s_wifi_boot_ticket, 0, sizeof(s_wifi_boot_ticket));
}

static bool app_wifi_boot_stage_allowed(uint16_t stage)
{
    return stage == (uint16_t)APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP ||
           stage == (uint16_t)APP_WIFI_APPLY_STAGE_STATIC_SET_IP;
}

static bool app_wifi_boot_ticket_valid(const app_wifi_boot_ticket_t *t)
{
    if (t->magic != APP_WIFI_BOOT_TICKET_MAGIC ||
        t->version != APP_WIFI_BOOT_TICKET_VERSION ||
        t->crc != app_wifi_boot_ticket_crc(t)) {
        return false;
    }
    if (t->stage == APP_WIFI_BOOT_TICKET_PROBE_STAGE) {
        return t->count == 0;
    }
    return app_wifi_boot_stage_allowed(t->stage) && t->count == 1;
}

static esp_err_t app_wifi_boot_ticket_write(uint16_t stage, uint8_t count)
{
    app_wifi_boot_ticket_t t = {0};
    bool pending;

    if (stage == APP_WIFI_BOOT_TICKET_PROBE_STAGE) {
        if (count != 0) {
            return ESP_ERR_INVALID_ARG;
        }
    } else if (!app_wifi_boot_stage_allowed(stage) || count != 1) {
        return ESP_ERR_INVALID_ARG;
    }
    t.magic = APP_WIFI_BOOT_TICKET_MAGIC;
    t.version = APP_WIFI_BOOT_TICKET_VERSION;
    t.stage = stage;
    t.count = count;
    t.crc = app_wifi_boot_ticket_crc(&t);

    /* 有未消费的有效票据时禁止覆盖(空/无效票据允许覆盖) */
    portENTER_CRITICAL(&s_wifi_hook_mux);
    pending = app_wifi_boot_ticket_valid(&s_wifi_boot_ticket);
    if (!pending) {
        memcpy(&s_wifi_boot_ticket, &t, sizeof(t));
    }
    portEXIT_CRITICAL(&s_wifi_hook_mux);
    if (pending) {
        ESP_LOGW(TAG, "boot_ticket_busy stage=%u", (unsigned)stage);
        return ESP_ERR_INVALID_STATE;
    }
    ESP_LOGW(TAG, "boot_ticket_armed stage=%u count=%u", (unsigned)stage, (unsigned)count);
    return ESP_OK;
}

/* 启动最早调用:读→校验→清 RTC→复制到 RAM(票据一次性) */
static void app_wifi_boot_ticket_consume(void)
{
    app_wifi_boot_ticket_t t;

    /* 读+清同一临界区,避免与运行期写入交错 */
    portENTER_CRITICAL(&s_wifi_hook_mux);
    memcpy(&t, &s_wifi_boot_ticket, sizeof(t));
    app_wifi_boot_ticket_clear();
    portEXIT_CRITICAL(&s_wifi_hook_mux);

    if (t.magic != APP_WIFI_BOOT_TICKET_MAGIC ||
        t.version != APP_WIFI_BOOT_TICKET_VERSION ||
        t.crc != app_wifi_boot_ticket_crc(&t)) {
        if (t.magic != 0 || t.version != 0 || t.stage != 0 || t.count != 0 ||
            t.reserved[0] != 0 || t.reserved[1] != 0 || t.reserved[2] != 0 ||
            t.crc != 0) {
            ESP_LOGW(TAG, "ticket_seen=1 invalid=1");
        }
        return;
    }
    if (t.stage == APP_WIFI_BOOT_TICKET_PROBE_STAGE && t.count == 0) {
        ESP_LOGW(TAG, "ticket_seen=1 probe=1");
        return;
    }
    if (!app_wifi_boot_stage_allowed(t.stage) || t.count != 1) {
        ESP_LOGW(TAG, "ticket_seen=1 invalid=1");
        return;
    }
    portENTER_CRITICAL(&s_wifi_hook_mux);
    bool override_ram = (s_wifi_hook.fail_count > 0);
    /* 规则:boot 票据优先。若 RAM 已有未消费注入(BLE 抢跑),以票据覆盖并记录 */
    s_wifi_hook.active = true;
    s_wifi_hook.fail_stage = (app_wifi_apply_stage_t)t.stage;
    s_wifi_hook.fail_count = 1;
    portEXIT_CRITICAL(&s_wifi_hook_mux);
    if (override_ram) {
        ESP_LOGW(TAG, "ticket_seen=1 stage=%s count=1 injected=1 override=1",
                 app_wifi_apply_stage_name((app_wifi_apply_stage_t)t.stage));
    } else {
        ESP_LOGW(TAG, "ticket_seen=1 stage=%s count=1 injected=1",
                 app_wifi_apply_stage_name((app_wifi_apply_stage_t)t.stage));
    }
}
static bool app_wifi_test_take_failure(app_wifi_apply_stage_t stage);
static bool app_wifi_test_take_stop_failure(void);
static void app_wifi_test_connect_hold_wait(const char *path);
static void app_wifi_boot_ticket_consume(void);
static const char *app_wifi_apply_stage_name(app_wifi_apply_stage_t stage);
static esp_err_t app_wifi_boot_ticket_write(uint16_t stage, uint8_t count);
#endif

/* 前向声明:该函数定义在断连原因码表附近(约 800 行),而连接路径(约 600 行)先用到它 */
static const char *app_wifi_auth_name(uint8_t auth);
static void app_wifi_log_sta_cfg(const char *where, const wifi_config_t *cfg);
static esp_err_t app_wifi_retry_with_pmf_required(void);

/* ===== M1 启动安全(设计稿 静态IP_M1启动安全设计稿_2026-11-03.md §3)=====
 * 定义分散在原 apply/static 配置区与恢复链区;先集中声明,调用点遍布本文件。 */
static esp_err_t app_configure_sta_static_ip(app_wifi_apply_stage_t *stage_out);
static esp_err_t app_wifi_apply_cfg(const app_wifi_apply_cfg_t *cfg,
                                    app_wifi_apply_stage_t *stage_out);
static void app_wifi_build_cfg_from_config(app_wifi_apply_cfg_t *out);
static void app_wifi_lkg_capture(const app_wifi_apply_cfg_t *cfg);
static bool app_wifi_lkg_equals(const app_wifi_apply_cfg_t *cfg);
static bool app_wifi_connect_blocked(void);
static bool app_wifi_connect_begin(const char *path);
static void app_wifi_connect_end(const char *path);
static bool app_wifi_wait_connect_inflight(int64_t limit_ms, bool recovery);
static bool app_wifi_tryconnect_entry_blocked(void);
static void app_wifi_apply_hold_clear(void);
static void app_wifi_offline_gate_clear_if_set(void);
static bool app_wifi_op_claim_apply(uint32_t wait_ms);
static void app_wifi_op_release_apply(void);
static bool app_wifi_recover_quiesce(void);
static void app_wifi_enter_offline(void);
static void app_wifi_recover_from_apply_failure(const app_wifi_apply_cfg_t *failed);
static void app_wifi_boot_apply_or_recover(void);
static void app_wifi_set_cfg_state(app_net_cfg_state_t state);
static void app_wifi_record_apply_failure(app_wifi_apply_stage_t stage, esp_err_t err,
                                          bool static_mode);

/* 210(NO_AP_FOUND_W_COMPAT_SECURITY)兜底:首轮 required=false,失败后带 required=true 只重试一次 */
static bool s_wifi_pmf_required_tried = false;

/* 试连验证状态(1.3-37):tryconnect 从取得 ownership 到 teardown 完成,
 * 独占 Wi-Fi 连接操作; 只有明确归属当前候选的事件才能修改结果。
 * 旧链路清理/预扫描窗口/候选连接窗口必须由 phase 明确分开。 */
typedef enum {
    APP_WIFI_TRY_PHASE_IDLE = 0,
    APP_WIFI_TRY_PHASE_PRE_SCAN,
    APP_WIFI_TRY_PHASE_CLEANUP_OLD,
    APP_WIFI_TRY_PHASE_WAIT_OLD_DISCONNECT,
    APP_WIFI_TRY_PHASE_CANDIDATE_STARTING,
    APP_WIFI_TRY_PHASE_CANDIDATE_CONNECTING,
    APP_WIFI_TRY_PHASE_DONE,
} app_wifi_try_phase_t;

typedef enum {
    APP_WIFI_TRY_MATCH_OLD = 0,
    APP_WIFI_TRY_MATCH_CANDIDATE,
    APP_WIFI_TRY_MATCH_UNKNOWN,
} app_wifi_try_match_t;

static bool s_tryconnect_active = false;
static app_wifi_try_phase_t s_tryconnect_phase = APP_WIFI_TRY_PHASE_IDLE;
static bool s_tryconnect_cancel_requested = false;
static bool s_tryconnect_require_ip = false;
static app_wifi_try_result_t s_tryconnect_result = APP_WIFI_TRY_TIMEOUT;
static char s_tryconnect_candidate_ssid[33] = {0};
static uint8_t s_tryconnect_candidate_ssid_len = 0;
static uint8_t s_tryconnect_candidate_bssid[6] = {0};
static bool s_tryconnect_candidate_bssid_valid = false;
static EventGroupHandle_t s_tryconnect_events = NULL;
static StaticEventGroup_t s_tryconnect_events_buf;
static portMUX_TYPE s_tryconnect_mux = portMUX_INITIALIZER_UNLOCKED;
#define APP_WIFI_TRY_DONE_BIT   BIT0
#define APP_WIFI_TRY_OLD_DISC_BIT BIT1

static void app_wifi_tryconnect_state_lock(void)
{
    portENTER_CRITICAL(&s_tryconnect_mux);
}

static void app_wifi_tryconnect_state_unlock(void)
{
    portEXIT_CRITICAL(&s_tryconnect_mux);
}

static bool app_wifi_tryconnect_is_active(void)
{
    bool active;

    app_wifi_tryconnect_state_lock();
    active = s_tryconnect_active && s_tryconnect_phase != APP_WIFI_TRY_PHASE_IDLE;
    app_wifi_tryconnect_state_unlock();
    return active;
}

static app_wifi_try_phase_t app_wifi_tryconnect_get_phase(void)
{
    app_wifi_try_phase_t phase;

    app_wifi_tryconnect_state_lock();
    phase = s_tryconnect_phase;
    app_wifi_tryconnect_state_unlock();
    return phase;
}

static app_wifi_try_result_t app_wifi_tryconnect_get_result(void)
{
    app_wifi_try_result_t result;

    app_wifi_tryconnect_state_lock();
    result = s_tryconnect_result;
    app_wifi_tryconnect_state_unlock();
    return result;
}

static bool app_wifi_tryconnect_cancel_get(void)
{
    bool cancel;

    app_wifi_tryconnect_state_lock();
    cancel = s_tryconnect_cancel_requested;
    app_wifi_tryconnect_state_unlock();
    return cancel;
}

static bool app_wifi_tryconnect_begin(const char *ssid, bool require_ip)
{
    bool ok = false;

    app_wifi_tryconnect_state_lock();
    if (!s_tryconnect_active && s_tryconnect_phase == APP_WIFI_TRY_PHASE_IDLE &&
        s_wifi_op_owner == APP_WIFI_OP_NONE) {
        s_wifi_op_owner = APP_WIFI_OP_TRYX;   /* A8:claim 与状态取得同一临界区 */
        s_tryconnect_active = true;
        s_tryconnect_phase = APP_WIFI_TRY_PHASE_PRE_SCAN;
        s_tryconnect_cancel_requested = false;
        s_tryconnect_require_ip = require_ip;
        s_tryconnect_result = APP_WIFI_TRY_TIMEOUT;
        app_copy_string(s_tryconnect_candidate_ssid, sizeof(s_tryconnect_candidate_ssid), ssid);
        s_tryconnect_candidate_ssid_len =
            (uint8_t)strlen(s_tryconnect_candidate_ssid);
        memset(s_tryconnect_candidate_bssid, 0, sizeof(s_tryconnect_candidate_bssid));
        s_tryconnect_candidate_bssid_valid = false;
        ok = true;
    }
    app_wifi_tryconnect_state_unlock();
    return ok;
}

static void app_wifi_tryconnect_release(void)
{
    app_wifi_tryconnect_state_lock();
    if (s_wifi_op_owner == APP_WIFI_OP_TRYX) {
        s_wifi_op_owner = APP_WIFI_OP_NONE;   /* A8:finish 唯一释放点 */
    }
    s_tryconnect_active = false;
    s_tryconnect_phase = APP_WIFI_TRY_PHASE_IDLE;
    s_tryconnect_cancel_requested = false;
    s_tryconnect_require_ip = false;
    s_tryconnect_candidate_ssid[0] = '\0';
    s_tryconnect_candidate_ssid_len = 0;
    memset(s_tryconnect_candidate_bssid, 0, sizeof(s_tryconnect_candidate_bssid));
    s_tryconnect_candidate_bssid_valid = false;
    app_wifi_tryconnect_state_unlock();
}

static void app_wifi_tryconnect_set_phase(app_wifi_try_phase_t phase)
{
    app_wifi_tryconnect_state_lock();
    s_tryconnect_phase = phase;
    app_wifi_tryconnect_state_unlock();
}

static void app_wifi_tryconnect_set_result_done(app_wifi_try_result_t result)
{
    app_wifi_tryconnect_state_lock();
    if (s_tryconnect_phase != APP_WIFI_TRY_PHASE_IDLE &&
        s_tryconnect_phase != APP_WIFI_TRY_PHASE_DONE) {
        s_tryconnect_result = result;
        s_tryconnect_phase = APP_WIFI_TRY_PHASE_DONE;
    }
    app_wifi_tryconnect_state_unlock();
}

#define APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS ((unsigned)(sizeof(s_wifi_attempted_mask) * 8U))

extern app_config_t s_config;

static esp_netif_t *s_sta_netif;
wifi_runtime_t s_wifi = {0};
static TaskHandle_t s_wifi_connect_task = NULL;
static TaskHandle_t s_wifi_boot_monitor_task = NULL;
static app_wifi_profile_t s_wifi_profiles[APP_WIFI_PROFILE_MAX];
static size_t s_wifi_profile_count = 0;
static bool s_wifi_profiles_dirty = true;
static int s_wifi_active_profile_index = -1;
static uint8_t s_wifi_attempted_mask = 0;
static int64_t s_wifi_startup_retry_at_ms = 0;
static int64_t s_sta_connected_at_ms = 0;
static bool s_wifi_sta_connecting = false;
static bool s_wifi_fast_path_pending = false;
/* 与 fast_path_pending 配套:为 true 时 connect task 只走正式 SSID,
 * 失败也不进入 profile 扫描(BLE 连接期间的降级路径)。 */
static bool s_wifi_formal_only_pending = false;
static SemaphoreHandle_t s_wifi_restart_lock = NULL;
static StaticSemaphore_t s_wifi_restart_lock_buf;
static portMUX_TYPE s_wifi_restart_lock_mux = portMUX_INITIALIZER_UNLOCKED;
/* BLE 救援命令独占 Wi-Fi:暂停新的自动重连调度,等待当前 connect task 退出,
 * 并串行化扫描,避免 STA 失败重连时 network.scan 返回空列表。 */
static volatile bool s_wifi_auto_connect_suspended = false;
static SemaphoreHandle_t s_wifi_scan_lock = NULL;
static StaticSemaphore_t s_wifi_scan_lock_buf;
static portMUX_TYPE s_wifi_scan_lock_mux = portMUX_INITIALIZER_UNLOCKED;
_Static_assert(sizeof(s_wifi.scan_records_storage) >=
                   sizeof(wifi_ap_record_t) * WIFI_SCAN_LIST_SIZE,
               "scan_records_storage must fit WIFI_SCAN_LIST_SIZE records");

_Static_assert(sizeof(TaskHandle_t) == sizeof(((wifi_runtime_t *)0)->restart_task),
               "TaskHandle_t must be same size as restart_task pointer");

static void app_wifi_restart_lock(void)
{
    SemaphoreHandle_t lock = app_mutex_ensure(&s_wifi_restart_lock, &s_wifi_restart_lock_buf,
                                              &s_wifi_restart_lock_mux, true);
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

static void app_wifi_restart_unlock(void)
{
    if (s_wifi_restart_lock != NULL) {
        xSemaphoreGiveRecursive(s_wifi_restart_lock);
    }
}

static void app_wifi_scan_lock(void)
{
    SemaphoreHandle_t lock = app_mutex_ensure(&s_wifi_scan_lock, &s_wifi_scan_lock_buf,
                                              &s_wifi_scan_lock_mux, true);
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

static void app_wifi_scan_unlock(void)
{
    if (s_wifi_scan_lock != NULL) {
        xSemaphoreGiveRecursive(s_wifi_scan_lock);
    }
}

#define S_WIFI_SCAN_RECORDS \
    ((wifi_ap_record_t *)s_wifi.scan_records_storage)

void app_wifi_mark_profiles_dirty(void)
{
    app_wifi_profiles_lock();
    s_wifi_profiles_dirty = true;
    app_wifi_profiles_unlock();
}

bool app_wifi_scan_record_at(uint16_t index,
                             const char **ssid_out,
                             int8_t *rssi_out,
                             uint8_t *authmode_out)
{
    wifi_ap_record_t *records = S_WIFI_SCAN_RECORDS;
    bool ok = false;
    app_wifi_runtime_lock();
    if (index < s_wifi.scan_count) {
        if (ssid_out != NULL) {
            *ssid_out = (const char *)records[index].ssid;
        }
        if (rssi_out != NULL) {
            *rssi_out = records[index].rssi;
        }
        if (authmode_out != NULL) {
            *authmode_out = (uint8_t)records[index].authmode;
        }
        ok = true;
    }
    app_wifi_runtime_unlock();
    return ok;
}

bool app_wifi_scan_record_bssid(uint16_t index, char *out, size_t out_size)
{
    wifi_ap_record_t *records = S_WIFI_SCAN_RECORDS;
    bool ok = false;

    if (out == NULL || out_size < 18) {
        return false;
    }
    out[0] = '\0';
    app_wifi_runtime_lock();
    if (index < s_wifi.scan_count) {
        const uint8_t *b = records[index].bssid;
        snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5]);
        ok = true;
    }
    app_wifi_runtime_unlock();
    return ok;
}

void app_wifi_sta_config_updated(void)
{
    app_wifi_mark_profiles_dirty();
    app_wifi_profiles_lock();
    s_wifi_attempted_mask = 0;
    s_wifi_active_profile_index = -1;
    app_wifi_profiles_unlock();
}

/* AP/APSTA strings exist only to read legacy NVS values; runtime forces STA and
 * cfg_validate_net_mode() rejects any non-STA write. */
const char *app_network_mode_to_string(uint8_t mode)
{
    switch (mode) {
    case NET_MODE_STA:
        return "sta";
    case NET_MODE_APSTA:
        return "apsta";
    case NET_MODE_AP:
    default:
        return "ap";
    }
}

uint8_t app_network_mode_from_string(const char *mode)
{
    if (strcmp(mode, "sta") == 0) {
        return NET_MODE_STA;
    }
    if (strcmp(mode, "apsta") == 0) {
        return NET_MODE_APSTA;
    }
    return NET_MODE_AP;
}

static void app_set_sta_ip_strings(const esp_netif_ip_info_t *ip_info)
{
    app_wifi_runtime_lock();
    if (ip_info == NULL) {
        app_copy_string(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), "0.0.0.0");
        app_copy_string(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), "0.0.0.0");
        app_copy_string(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), "0.0.0.0");
        app_wifi_runtime_unlock();
        return;
    }

    snprintf(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), IPSTR, IP2STR(&ip_info->ip));
    snprintf(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), IPSTR, IP2STR(&ip_info->gw));
    snprintf(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), IPSTR, IP2STR(&ip_info->netmask));
    app_wifi_runtime_unlock();
}

/* ---------------------------------------------------------------------------
 * 静态 IP 冲突检测(RFC 5227 简化版):拿到静态 IP 且 STA 连上后,对自身 IP
 * 发 ARP 请求;若局域网里有其它设备应答(ARP 表里出现非自身 MAC 的条目),
 * 即判定冲突,置 s_wifi.sta_ip_conflict —— 状态接口透出,前端弹窗提示改址。
 * ------------------------------------------------------------------------- */
static TaskHandle_t s_ip_conflict_task = NULL;

static void app_sta_ip_conflict_check_task(void *arg)
{
    (void)arg;
    esp_netif_ip_info_t ip_info;
    ip4_addr_t probe_ip;
    struct eth_addr *entry_mac = NULL;
    const ip4_addr_t *entry_ip = NULL;
    uint8_t own_mac[6] = {0};
    char ip_str[16];
    bool conflict = false;
    int i;

    vTaskDelay(pdMS_TO_TICKS(400));
    if (s_sta_netif == NULL ||
        esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        goto done;
    }
    {
        struct netif *netif_lwip = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);

        if (netif_lwip == NULL) {
            goto done;
        }
        probe_ip.addr = ip_info.ip.addr;
        for (i = 0; i < 3; i++) {
            etharp_request(netif_lwip, &probe_ip);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        if (etharp_find_addr(netif_lwip, &probe_ip, &entry_mac, &entry_ip) >= 0 &&
            entry_mac != NULL) {
            if (esp_wifi_get_mac(WIFI_IF_STA, own_mac) == ESP_OK &&
                memcmp(entry_mac->addr, own_mac, sizeof(own_mac)) == 0) {
                ESP_LOGI(TAG, "static ip %s: ARP entry is own MAC, no conflict", ip_str);
            } else {
                conflict = true;
                ESP_LOGW(TAG, "static ip CONFLICT: %s answered by %02x:%02x:%02x:%02x:%02x:%02x",
                         ip_str, entry_mac->addr[0], entry_mac->addr[1], entry_mac->addr[2],
                         entry_mac->addr[3], entry_mac->addr[4], entry_mac->addr[5]);
            }
        } else {
            ESP_LOGI(TAG, "static ip %s: no ARP answer, no conflict", ip_str);
        }
    }

done:
    app_wifi_runtime_lock();
    s_wifi.sta_ip_conflict = conflict;
    app_wifi_runtime_unlock();
    s_ip_conflict_task = NULL;
    vTaskDelete(NULL);
}

static void app_sta_ip_conflict_check_start(void)
{
    if (s_ip_conflict_task != NULL) {
        return;
    }
    if (xTaskCreate(app_sta_ip_conflict_check_task, "ip_conflict", 2560, NULL, 3,
                    &s_ip_conflict_task) != pdPASS) {
        s_ip_conflict_task = NULL;
        ESP_LOGW(TAG, "Failed to create static ip conflict check task");
    }
}

bool app_network_mode_has_sta(uint8_t mode)
{
    (void)mode;
    return true;
}

static int64_t app_wifi_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void app_wifi_set_startup_fallback_reason(const char *reason)
{
    app_wifi_runtime_lock();
    app_copy_string(s_wifi.startup_fallback_reason,
                    sizeof(s_wifi.startup_fallback_reason),
                    reason != NULL ? reason : "none");
    app_wifi_runtime_unlock();
}

static void app_wifi_stop_startup_connect_window(void)
{
    app_wifi_runtime_lock();
    s_wifi.startup_connect_active = false;
    s_wifi.startup_connect_deadline_ms = 0;
    s_wifi_fast_path_pending = false;
    s_wifi_formal_only_pending = false;
    app_wifi_runtime_unlock();
    s_wifi_startup_retry_at_ms = 0;
    s_wifi_sta_connecting = false;
    s_wifi_rt_armed = true;   /* 启动阶段结束,运行时兜底自此常驻 */
    s_wifi_ble_fast_retry_at_ms = 0;
    s_wifi_ble_fast_cycle = 0;
}

static void app_wifi_start_startup_connect_window(void)
{
    uint8_t net_mode;
    app_config_lock();
    net_mode = s_config.net_mode;
    app_config_unlock();
    if (net_mode != NET_MODE_STA) {
        app_wifi_stop_startup_connect_window();
        return;
    }

    app_wifi_runtime_lock();
    s_wifi.startup_connect_active = true;
    s_wifi.startup_connect_deadline_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_TIMEOUT_MS;
    s_wifi_fast_path_pending = true;
    s_wifi_formal_only_pending = false;
    app_wifi_runtime_unlock();
    s_wifi_startup_retry_at_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
    /* 新的连通性纪元(启动/apply 重启):运行时兜底退避归零 */
    s_wifi_rt_retry_at_ms = 0;
    s_wifi_rt_retry_step = 0;
    s_wifi_rt_retry_cycle = 0;
    s_wifi_ble_fast_retry_at_ms = 0;
    s_wifi_ble_fast_cycle = 0;
    app_wifi_set_startup_fallback_reason("none");
}

static bool app_parse_ipv4_str(const char *s, esp_ip4_addr_t *out)
{
    ip4_addr_t parsed;

    if (s == NULL || out == NULL) {
        return false;
    }
    if (!ip4addr_aton(s, &parsed) || ip4_addr_isany_val(parsed)) {
        return false;
    }
    out->addr = parsed.addr;
    return true;
}

static esp_err_t app_configure_sta_static_ip(app_wifi_apply_stage_t *stage_out)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dns_info_t dns_info = {0};
    char ip_str[sizeof(s_config.sta_static_ip)];
    char gw_str[sizeof(s_config.sta_static_gateway)];
    char mask_str[sizeof(s_config.sta_static_netmask)];
    char dns1_str[sizeof(s_config.sta_static_dns1)];
    char dns2_str[sizeof(s_config.sta_static_dns2)];
    esp_err_t err;

    if (s_sta_netif == NULL) {
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_STATIC_PARSE;
        }
        return ESP_ERR_INVALID_STATE;
    }

    app_config_lock();
    app_copy_string(ip_str, sizeof(ip_str), s_config.sta_static_ip);
    app_copy_string(gw_str, sizeof(gw_str), s_config.sta_static_gateway);
    app_copy_string(mask_str, sizeof(mask_str), s_config.sta_static_netmask);
    app_copy_string(dns1_str, sizeof(dns1_str), s_config.sta_static_dns1);
    app_copy_string(dns2_str, sizeof(dns2_str), s_config.sta_static_dns2);
    app_config_unlock();

#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_STATIC_PARSE)) {
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_STATIC_PARSE;
        }
        return ESP_ERR_INVALID_ARG;
    }
#endif
    if (!app_parse_ipv4_str(ip_str, &ip_info.ip) ||
        !app_parse_ipv4_str(gw_str, &ip_info.gw) ||
        !app_parse_ipv4_str(mask_str, &ip_info.netmask)) {
        ESP_LOGE(TAG, "static ip parse failed");
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_STATIC_PARSE;
        }
        return ESP_ERR_INVALID_ARG;
    }

    /* M1-04(2026-11-02 裁决):dhcpc_stop 失败立即中止静态应用,不得带病
     * 继续 set_ip_info —— DHCP 客户端未停止时写静态地址会让 netif 处于
     * 不一致状态。失败进入恢复决策链,由恢复链决定救援路径。 */
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP)) {
        if (stage_out != NULL) *stage_out = APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP;
        return ESP_FAIL;
    }
#endif
    err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGE(TAG, "static ip: dhcpc_stop failed: %s", esp_err_to_name(err));
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP;
        }
        return err;
    }

#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_STATIC_SET_IP)) {
        if (stage_out != NULL) *stage_out = APP_WIFI_APPLY_STAGE_STATIC_SET_IP;
        return ESP_FAIL;
    }
#endif
    err = esp_netif_set_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static ip: set_ip_info failed: %s", esp_err_to_name(err));
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_STATIC_SET_IP;
        }
        return err;
    }

    if (dns1_str[0] != '\0' && app_parse_ipv4_str(dns1_str, &dns_info.ip.u_addr.ip4)) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    if (dns2_str[0] != '\0' && app_parse_ipv4_str(dns2_str, &dns_info.ip.u_addr.ip4)) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns_info);
    }

    ESP_LOGI(TAG, "static ip configured: ip=%s gw=%s mask=%s dns1=%s dns2=%s",
             ip_str, gw_str, mask_str,
             dns1_str[0] != '\0' ? dns1_str : "(none)",
             dns2_str[0] != '\0' ? dns2_str : "(none)");
    return ESP_OK;
}

static void app_wifi_load_saved_profiles(void)
{
    app_wifi_profile_t stored_profiles[APP_WIFI_PROFILE_MAX] = {0};
    size_t stored_count = 0;
    size_t read_index;
    size_t write_index = 0;
    char cfg_sta_ssid[sizeof(s_config.sta_ssid)];
    char cfg_sta_password[sizeof(s_config.sta_password)];
    bool cfg_sta_has_ssid;

    app_config_lock();
    cfg_sta_has_ssid = (s_config.sta_ssid[0] != '\0');
    if (cfg_sta_has_ssid) {
        app_copy_string(cfg_sta_ssid, sizeof(cfg_sta_ssid), s_config.sta_ssid);
        app_copy_string(cfg_sta_password, sizeof(cfg_sta_password), s_config.sta_password);
    } else {
        cfg_sta_ssid[0] = '\0';
        cfg_sta_password[0] = '\0';
    }
    app_config_unlock();

    app_wifi_profiles_lock();
    if (!s_wifi_profiles_dirty) {
        app_wifi_profiles_unlock();
        return;
    }

    memset(s_wifi_profiles, 0, sizeof(s_wifi_profiles));
    s_wifi_profile_count = 0;
    app_wifi_profiles_unlock();
    if (app_config_load_wifi_profiles(stored_profiles, APP_WIFI_PROFILE_MAX, &stored_count) != ESP_OK) {
        stored_count = 0;
    }
    if (stored_count > APP_WIFI_PROFILE_MAX) {
        stored_count = APP_WIFI_PROFILE_MAX;
    }

    app_wifi_profiles_lock();
    if (cfg_sta_has_ssid) {
        app_copy_string(s_wifi_profiles[write_index].ssid,
                        sizeof(s_wifi_profiles[write_index].ssid),
                        cfg_sta_ssid);
        app_copy_string(s_wifi_profiles[write_index].password,
                        sizeof(s_wifi_profiles[write_index].password),
                        cfg_sta_password);
        write_index++;
    }

    for (read_index = 0;
         read_index < stored_count && write_index < APP_WIFI_PROFILE_MAX;
         read_index++) {
        if (stored_profiles[read_index].ssid[0] == '\0') {
            continue;
        }
        if (cfg_sta_has_ssid &&
            strcmp(stored_profiles[read_index].ssid, cfg_sta_ssid) == 0) {
            continue;
        }
        s_wifi_profiles[write_index] = stored_profiles[read_index];
        write_index++;
    }

    s_wifi_profile_count = write_index;
    s_wifi_profiles_dirty = false;
    app_wifi_profiles_unlock();
}

static int app_wifi_find_best_rssi_for_profile(const char *ssid)
{
    int best_rssi = -128;
    uint16_t index;
    uint16_t scan_count;

    if (ssid == NULL || ssid[0] == '\0') {
        return -128;
    }
    app_wifi_runtime_lock();
    scan_count = s_wifi.scan_count;
    app_wifi_runtime_unlock();
    for (index = 0; index < scan_count; index++) {
        if (strcmp((const char *)S_WIFI_SCAN_RECORDS[index].ssid, ssid) != 0) {
            continue;
        }
        if (S_WIFI_SCAN_RECORDS[index].rssi > best_rssi) {
            best_rssi = S_WIFI_SCAN_RECORDS[index].rssi;
        }
    }
    return best_rssi;
}

static int app_wifi_select_best_profile_index(uint8_t exclude_mask)
{
    int best_index = -1;
    int best_rssi = -128;
    size_t index;
    size_t profile_count;
    size_t mask_bits;

    app_wifi_profiles_lock();
    profile_count = s_wifi_profile_count;
    app_wifi_profiles_unlock();
    mask_bits = APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS;

    for (index = 0;
         index < profile_count && index < mask_bits;
         index++) {
        int rssi;
        const char *ssid;

        if ((exclude_mask & (1U << index)) != 0U) {
            continue;
        }
        app_wifi_profiles_lock();
        ssid = s_wifi_profiles[index].ssid;
        app_wifi_profiles_unlock();
        rssi = app_wifi_find_best_rssi_for_profile(ssid);
        if (rssi <= -128) {
            continue;
        }
        if (best_index < 0 || rssi > best_rssi || (rssi == best_rssi && (int)index < best_index)) {
            best_index = (int)index;
            best_rssi = rssi;
        }
    }
    return best_index;
}

static void app_wifi_handle_startup_sta_failure(const char *reason);
static bool app_wifi_schedule_connect_task(TickType_t delay_ticks);

/* 记录"实际连上的 SSID"。
 * 取 esp_wifi_sta_get_ap_info() 的关联结果而不是 s_wifi_active_profile_index：
 * fast_path 未在 profiles 里命中时 index 会保留上一次的值，直接暴露会误报。
 * connected_profile 按 SSID 反查下标，查不到即 -1（例如正式 SSID 不在 profiles 里）。 */
static void app_wifi_set_connected_ssid(const char *ssid)
{
    int matched = -1;
    size_t i;
    size_t count;

    if (ssid == NULL) {
        ssid = "";
    }

    app_wifi_profiles_lock();
    count = s_wifi_profile_count;
    for (i = 0; i < count; i++) {
        if (strcmp((const char *)s_wifi_profiles[i].ssid, ssid) == 0) {
            matched = (int)i;
            break;
        }
    }
    app_wifi_profiles_unlock();

    app_wifi_runtime_lock();
    app_copy_string(s_wifi.connected_ssid, sizeof(s_wifi.connected_ssid), ssid);
    s_wifi.connected_profile = matched;
    app_wifi_runtime_unlock();
}

static esp_err_t app_wifi_connect_profile_index(int profile_index)
{
    wifi_config_t sta_cfg = {0};
    esp_err_t err;
    size_t profile_count;
    int active_snapshot;
    bool same_profile;

    app_wifi_profiles_lock();
    profile_count = s_wifi_profile_count;
    active_snapshot = s_wifi_active_profile_index;
    app_wifi_profiles_unlock();

    if (profile_index < 0 || (size_t)profile_index >= profile_count) {
        return ESP_ERR_INVALID_ARG;
    }

    same_profile = (active_snapshot == profile_index);
    if (!same_profile) {
        app_wifi_profiles_lock();
        app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid),
                             s_wifi_profiles[profile_index].ssid);
        app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password),
                             s_wifi_profiles[profile_index].password);
        app_wifi_profiles_unlock();
        sta_cfg.sta.threshold.authmode =
            (strlen((const char *)sta_cfg.sta.password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }

    /* M1-A9:许可覆盖完整驱动区间(disconnect/set_config/connect),
     * 使 apply drain 等待整段操作,而不是仅最后一次 connect */
    if (!app_wifi_connect_begin("profile")) {
        s_wifi_sta_connecting = false;
        return ESP_ERR_INVALID_STATE;
    }
    esp_wifi_disconnect();
    if (!same_profile) {
        app_wifi_log_sta_cfg("connect_profile", &sta_cfg);
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            s_wifi_sta_connecting = false;
            ESP_LOGE(TAG, "connect_profile: set sta config failed: %s", esp_err_to_name(err));
            app_wifi_connect_end("profile");
            return err;
        }
        app_wifi_profiles_lock();
        s_wifi_active_profile_index = profile_index;
        app_wifi_profiles_unlock();
    }
    app_wifi_runtime_lock();
    s_wifi.sta_retries = 0;
    app_wifi_runtime_unlock();
    s_wifi_sta_connecting = true;
    app_wifi_runtime_lock();
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "switching_profile");
    app_wifi_runtime_unlock();
    err = esp_wifi_connect();
    app_wifi_connect_end("profile");
    if (err != ESP_OK) {
        /* D1:同步失败没有断连事件,必须当场清零。 */
        s_wifi_sta_connecting = false;
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect),
                        "connect_failed");
        app_wifi_runtime_unlock();
        ESP_LOGW(TAG, "connect_profile: esp_wifi_connect failed: %s",
                 esp_err_to_name(err));
    }
    return err;
}

static esp_err_t app_wifi_connect_best_saved_profile(bool allow_fast_path,
                                                     bool allow_profile_scan)
{
    esp_err_t err;
    int profile_index;
    size_t profile_count;
    char sta_ssid_snapshot[33] = {0};
    char sta_password_snapshot[65] = {0};
    bool sta_has_ssid = false;
    bool formal_static = false;

    app_wifi_load_saved_profiles();
    app_config_lock();
    if (s_config.sta_ssid[0] != '\0') {
        app_copy_string(sta_ssid_snapshot, sizeof(sta_ssid_snapshot), s_config.sta_ssid);
        app_copy_string(sta_password_snapshot, sizeof(sta_password_snapshot), s_config.sta_password);
        sta_has_ssid = true;
    }
    formal_static = (s_config.sta_use_static != 0);
    app_config_unlock();

    /* 静态 IP 与正式 SSID 绑定；即便调用方不允许 fast path，也必须先试正式配置。 */
    if (formal_static) {
        allow_fast_path = true;
    }
    /* 不允许 profile 扫描时,唯一能做的自动动作就是正式 SSID fast path;
     * 即使调用方没置 fast_path_pending 也强制尝试一次。 */
    if (!allow_profile_scan) {
        allow_fast_path = true;
    }

    if (allow_fast_path && sta_has_ssid) {
        wifi_config_t sta_cfg = {0};
        app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), sta_ssid_snapshot);
        app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), sta_password_snapshot);
        sta_cfg.sta.threshold.authmode =
            (strlen(sta_password_snapshot) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
        sta_cfg.sta.bssid_set = false;
        sta_cfg.sta.channel = 0;
        app_wifi_log_sta_cfg("fast_path", &sta_cfg);
        app_wifi_profiles_lock();
        profile_index = -1;
        if (s_wifi_profile_count > 0) {
            size_t i;
            for (i = 0; i < s_wifi_profile_count; i++) {
                if (strcmp((const char *)s_wifi_profiles[i].ssid, sta_ssid_snapshot) == 0) {
                    profile_index = (int)i;
                    break;
                }
            }
        }
        app_wifi_profiles_unlock();
        /* M1-A9:许可覆盖 disconnect/set_config/connect 全区间 */
        if (!app_wifi_connect_begin("fast_path")) {
            s_wifi_sta_connecting = false;
            return ESP_ERR_INVALID_STATE;
        }
        esp_wifi_disconnect();
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err == ESP_OK) {
            if (profile_index >= 0) {
                app_wifi_profiles_lock();
                s_wifi_active_profile_index = profile_index;
                app_wifi_profiles_unlock();
            }
            app_wifi_runtime_lock();
            s_wifi.sta_retries = 0;
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = true;
            app_wifi_runtime_lock();
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "fast_path");
            app_wifi_runtime_unlock();
            err = esp_wifi_connect();
            app_wifi_connect_end("fast_path");
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "auto_connect: fast_path ssid=%s%s", sta_ssid_snapshot,
                         allow_profile_scan ? "" : " (formal-only, no scan)");
                return ESP_OK;
            }
            /* D1:同步失败没有后续断连事件,先清零再决定是否扫描回退。 */
            s_wifi_sta_connecting = false;
            ESP_LOGW(TAG, "fast_path connect failed: %s%s", esp_err_to_name(err),
                     allow_profile_scan ? ", fall back to scan" : " (formal-only)");
        } else {
            app_wifi_connect_end("fast_path");
            ESP_LOGW(TAG, "fast_path set_config failed: %s, fall back to scan", esp_err_to_name(err));
        }
    }

    if (formal_static) {
        /* 静态 IP 是给正式 SSID 的，不能扫描后切到另一个 profile：
         * 那会把 A 的静态地址套到 B 上（P0-4 的触发条件）。
         * fast path 失败时清掉 connecting，让 boot monitor 继续重试正式 SSID，
         * 而不是留下 true 后永久不调度。 */
        ESP_LOGW(TAG, "formal config uses static IP; skip saved-profile fallback (ssid=%s)",
                 sta_ssid_snapshot);
        s_wifi_sta_connecting = false;
        return ESP_ERR_INVALID_STATE;
    }

    if (!allow_profile_scan) {
        /* BLE 连接期间的降级路径:fast path 失败后不扫描 profile。
         * 必须清 connecting,否则 monitor 会永久等待一个不会到来的事件。 */
        s_wifi_sta_connecting = false;
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect),
                        "ble_fast_path_failed");
        app_wifi_runtime_unlock();
        ESP_LOGW(TAG, "formal-only retry: saved-profile scan skipped (BLE connected)");
        return ESP_ERR_NOT_FOUND;
    }

    app_wifi_profiles_lock();
    profile_count = s_wifi_profile_count;
    app_wifi_profiles_unlock();
    if (profile_count == 0) {
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_profile");
        app_wifi_runtime_unlock();
        ESP_LOGW(TAG, "no saved wifi profile: STA will not connect (AP-only until configured)");
        return ESP_ERR_NOT_FOUND;
    }

    err = app_wifi_perform_scan();
    if (err != ESP_OK) {
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "scan_failed");
        app_wifi_runtime_unlock();
        ESP_LOGW(TAG, "wifi scan failed (%s): cannot select saved AP", esp_err_to_name(err));
        return err;
    }

    for (;;) {
        uint8_t attempted_mask;
        app_wifi_profiles_lock();
        attempted_mask = s_wifi_attempted_mask;
        app_wifi_profiles_unlock();
        profile_index = app_wifi_select_best_profile_index(attempted_mask);
        if (profile_index < 0) {
            uint16_t scanned_snapshot;
            size_t pi;

            app_wifi_profiles_lock();
            s_wifi_attempted_mask = 0;
            app_wifi_profiles_unlock();
            app_wifi_runtime_lock();
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_ap");
            scanned_snapshot = s_wifi.scan_count;
            app_wifi_runtime_unlock();
            /* 这是"设备看起来卡死"的最常见真因:一切正常,只是扫描里没有已存的 AP */
            ESP_LOGW(TAG, "no untried saved AP left (profiles=%u, scanned=%u): STA stays down",
                     (unsigned)profile_count, (unsigned)scanned_snapshot);
            /* 1.3-04:把"为什么连不上"变成可自证的一行日志 ——
             * 命中扫描:给出 rssi/auth 与阈值的比较(安全模式不兼容/信号弱一目了然);
             * 未命中:明确说明 AP 不在扫描表里(关机/隐藏/超出距离)。
             * 否则现场只看到 reason=210(NO_AP_FOUND_W_COMPAT_SECURITY),无法判断。 */
            app_wifi_profiles_lock();
            for (pi = 0; pi < s_wifi_profile_count; pi++) {
                const char *wanted = (const char *)s_wifi_profiles[pi].ssid;
                uint16_t si;
                bool seen_any = false;

                for (si = 0; si < scanned_snapshot; si++) {
                    const char *seen_ssid = NULL;
                    int8_t seen_rssi = 0;
                    uint8_t seen_auth = 0;

                    if (!app_wifi_scan_record_at(si, &seen_ssid, &seen_rssi, &seen_auth)) {
                        continue;
                    }
                    if (seen_ssid == NULL || strcmp(seen_ssid, wanted) != 0) {
                        continue;
                    }
                    seen_any = true;
                    {
                        char seen_bssid[18] = "";
                        size_t seen_pwd_len = strlen(s_wifi_profiles[pi].password);
                        uint8_t min_auth = (seen_pwd_len >= 8) ? (uint8_t)APP_WIFI_STA_AUTHMODE_MIN
                                                               : (uint8_t)WIFI_AUTH_OPEN;

                        (void)app_wifi_scan_record_bssid(si, seen_bssid, sizeof(seen_bssid));
                        ESP_LOGW(TAG,
                                 "  profile[%u] '%s' in scan: bssid=%s rssi=%d auth=%u(%s) "
                                 "pwd_len=%u min=%u(%s) -> %s",
                                 (unsigned)pi, wanted, seen_bssid, (int)seen_rssi,
                                 (unsigned)seen_auth, app_wifi_auth_name(seen_auth),
                                 (unsigned)seen_pwd_len, (unsigned)min_auth,
                                 app_wifi_auth_name(min_auth),
                                 (seen_pwd_len == 0)
                                     ? "PASSWORD EMPTY -> STA looks like an OPEN client (210)"
                                     : (seen_auth < min_auth)
                                           ? "SECURITY BELOW THRESHOLD, will not connect"
                                           : "compatible: check password or signal");
                    }
                }
                if (!seen_any) {
                    ESP_LOGW(TAG, "  profile[%u] '%s' NOT in scan (AP off / hidden / out of range?)",
                             (unsigned)pi, wanted);
                }
            }
            app_wifi_profiles_unlock();
            return ESP_ERR_NOT_FOUND;
        }
        err = app_wifi_connect_profile_index(profile_index);
        if (err == ESP_OK) {
            app_wifi_profiles_lock();
            /* pwd_len 用于核对"主配置 vs profile"是否一致(1.3-21:排查口令首字符丢失) */
            ESP_LOGI(TAG, "auto_connect: selected saved profile index=%d ssid=%s pwd_len=%u",
                     profile_index, s_wifi_profiles[profile_index].ssid,
                     (unsigned)strlen(s_wifi_profiles[profile_index].password));
            app_wifi_profiles_unlock();
            return ESP_OK;
        }
        app_wifi_profiles_lock();
        s_wifi_attempted_mask |= (uint8_t)(1U << profile_index);
        app_wifi_profiles_unlock();
    }
}

static void app_wifi_connect_task_fn(void *arg)
{
    TickType_t delay_ticks = (TickType_t)(uintptr_t)arg;
    bool allow_fast_path = false;
    bool allow_profile_scan = true;

    if (delay_ticks > 0) {
        vTaskDelay(delay_ticks);
    }
    /* M1 R4:已排队任务延时结束后二次检查离线门控;被门控时直接退出,
     * 不改 s_wifi_sta_connecting,也不发起任何连接。 */
    if (app_wifi_connect_blocked()) {
        s_wifi_connect_task = NULL;
        vTaskDelete(NULL);
    }
    app_wifi_runtime_lock();
    allow_fast_path = s_wifi_fast_path_pending;
    allow_profile_scan = !s_wifi_formal_only_pending;
    s_wifi_fast_path_pending = false;
    s_wifi_formal_only_pending = false;
    app_wifi_runtime_unlock();
    app_wifi_connect_best_saved_profile(allow_fast_path, allow_profile_scan);
    s_wifi_connect_task = NULL;
    vTaskDelete(NULL);
}

static void app_wifi_handle_startup_sta_failure(const char *reason)
{
    const char *effective_reason = reason;
    char buf_last_disconnect[sizeof(s_wifi.last_disconnect)];
    char buf_fallback_reason[sizeof(s_wifi.startup_fallback_reason)];

    app_wifi_runtime_lock();
    app_copy_string(buf_last_disconnect, sizeof(buf_last_disconnect), s_wifi.last_disconnect);
    app_wifi_runtime_unlock();
    if (buf_last_disconnect[0] != '\0' &&
        strcmp(buf_last_disconnect, "none") != 0 &&
        strcmp(buf_last_disconnect, "switching_profile") != 0) {
        effective_reason = buf_last_disconnect;
    }

    /* HTTP/Web 已删除:AP 没有配置通道。启动失败保持 STA,让 BLE 继续广播
     * 供小程序救援;不再自动切 AP,也不再把 net_mode 持久化为 AP。 */
    app_wifi_set_startup_fallback_reason(effective_reason != NULL ? effective_reason : "connect_timeout");
    app_wifi_stop_startup_connect_window();
    app_wifi_runtime_lock();
    app_copy_string(buf_fallback_reason, sizeof(buf_fallback_reason), s_wifi.startup_fallback_reason);
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), buf_fallback_reason);
    app_wifi_runtime_unlock();
    ESP_LOGW(TAG, "STA startup failed (%s); staying in STA for BLE rescue (AP fallback disabled)",
             buf_fallback_reason);
}

static void app_wifi_boot_monitor_task_fn(void *arg)
{
    (void)arg;

    while (true) {
        int64_t now_ms = app_wifi_now_ms();
        bool startup_connect_active;
        bool sta_has_ip;
        bool sta_connected;
        bool sta_connecting;
        bool schedule_connect_now = false;
        bool handle_failure = false;
        int64_t deadline_ms;
        uint8_t net_mode;
        TaskHandle_t connect_task_snapshot;
        bool ble_connected;
        bool cfg_offline;

        app_config_lock();
        net_mode = s_config.net_mode;
        app_config_unlock();
        app_wifi_runtime_lock();
        startup_connect_active = s_wifi.startup_connect_active;
        sta_has_ip = s_wifi.sta_has_ip;
        sta_connected = s_wifi.sta_connected;
        deadline_ms = s_wifi.startup_connect_deadline_ms;
        cfg_offline = s_wifi.cfg_offline;
        app_wifi_runtime_unlock();
        sta_connecting = s_wifi_sta_connecting;
        connect_task_snapshot = s_wifi_connect_task;
        /* D1 看门狗:只在没有用户 tryconnect/scan 占用时计时;
         * 标志 30s 没有被 connected/disconnected 事件回收就强制清零。 */
        if (!sta_connecting || app_wifi_tryconnect_is_active() || s_wifi_auto_connect_suspended) {
            s_wifi_connecting_since_ms = 0;
        } else if (s_wifi_connecting_since_ms == 0) {
            s_wifi_connecting_since_ms = now_ms;
        } else if (now_ms - s_wifi_connecting_since_ms >= APP_WIFI_CONNECT_STUCK_TIMEOUT_MS) {
            s_wifi_sta_connecting = false;
            s_wifi_connecting_since_ms = 0;
            s_wifi_rt_retry_at_ms = 0;
            s_wifi_startup_retry_at_ms = 0;
            s_wifi_ble_fast_retry_at_ms = 0;
            app_wifi_runtime_lock();
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect),
                            "connect_stuck");
            app_wifi_runtime_unlock();
            ESP_LOGW(TAG, "wifi connect watchdog: sta_connecting stuck >%lldms, cleared",
                     (long long)APP_WIFI_CONNECT_STUCK_TIMEOUT_MS);
            sta_connecting = false;
        }
        ble_connected = app_ble_gatts_connected();
        if (ble_connected != s_wifi_ble_was_connected) {
            s_wifi_ble_was_connected = ble_connected;
            s_wifi_ble_fast_retry_at_ms = 0;   /* 连接态跳变:重新计 120s */
            if (!ble_connected) {
                s_wifi_formal_only_pending = false;  /* 回到完整重试 */
            }
            ESP_LOGI(TAG, "ble fast retry: link %s, timer reset",
                     ble_connected ? "up" : "down");
        }

        if (startup_connect_active && net_mode == NET_MODE_STA && !sta_has_ip) {
            if (now_ms >= deadline_ms) {
                handle_failure = true;
            } else if (!sta_connected &&
                       !sta_connecting &&
                       connect_task_snapshot == NULL &&
                       now_ms >= s_wifi_startup_retry_at_ms) {
                schedule_connect_now = true;
            }
        } else if (s_wifi_rt_armed && !startup_connect_active && net_mode == NET_MODE_STA &&
                   !sta_connected && !sta_has_ip && !sta_connecting &&
                   connect_task_snapshot == NULL &&
                   !app_wifi_tryconnect_is_active() &&
                   !s_wifi_auto_connect_suspended &&
                   !cfg_offline &&   /* M1 R2:离线门控,禁止 monitor 兜底重连 */
                   !ble_connected) {
            /* 运行时断网兜底:复用 schedule_connect_task 单一入口(互斥自动生效),
             * 不在本循环里直接 esp_wifi_connect()。 */
            if (s_wifi_rt_retry_at_ms == 0 || now_ms >= s_wifi_rt_retry_at_ms) {
                int64_t backoff_ms = APP_WIFI_RUNTIME_RETRY_BACKOFF_MS[s_wifi_rt_retry_step];

                s_wifi_rt_retry_cycle += 1;
                ESP_LOGI(TAG, "runtime retry: cycle=%u step=%u backoff=%lldms ble_conn=0",
                         (unsigned)s_wifi_rt_retry_cycle,
                         (unsigned)s_wifi_rt_retry_step,
                         (long long)backoff_ms);
                app_wifi_profiles_lock();
                s_wifi_attempted_mask = 0;   /* 新一轮允许重试所有已存 profile */
                app_wifi_profiles_unlock();
                s_wifi_fast_path_pending = true;   /* 每轮先试正式 SSID(静态 IP 也走这里) */
                s_wifi_formal_only_pending = false; /* 完整周期允许 profile 扫描 */
                app_wifi_schedule_connect_task(0);
                s_wifi_rt_retry_at_ms = now_ms + backoff_ms;
                if (s_wifi_rt_retry_step < APP_WIFI_RUNTIME_RETRY_STEPS - 1) {
                    s_wifi_rt_retry_step += 1;
                }
            }
        } else if (s_wifi_rt_armed && !startup_connect_active && net_mode == NET_MODE_STA &&
                   !sta_connected && !sta_has_ip && !sta_connecting &&
                   connect_task_snapshot == NULL &&
                   !app_wifi_tryconnect_is_active() &&
                   !s_wifi_auto_connect_suspended &&
                   !cfg_offline &&   /* M1 R2:离线门控,BLE 期间 120s 重试同样禁止 */
                   ble_connected) {
            /* 产品决策 A(2026-11-01):BLE 连接期间只试正式 SSID,不扫描
             * profile;120s 一次。解决"配置完忘了关小程序/蓝牙,AP 恢复后
             * 永久离线"——原门控把链路存在当成用户在场。 */
            if (s_wifi_ble_fast_retry_at_ms == 0) {
                s_wifi_ble_fast_retry_at_ms = now_ms + APP_WIFI_BLE_FAST_RETRY_MS;
                ESP_LOGI(TAG, "ble fast retry: first attempt in %lldms "
                              "(formal SSID only, no scan)",
                         (long long)APP_WIFI_BLE_FAST_RETRY_MS);
            } else if (now_ms >= s_wifi_ble_fast_retry_at_ms) {
                s_wifi_ble_fast_cycle += 1;
                ESP_LOGI(TAG, "ble fast retry: cycle=%u (formal SSID only, no scan)",
                         (unsigned)s_wifi_ble_fast_cycle);
                s_wifi_fast_path_pending = true;
                s_wifi_formal_only_pending = true;
                app_wifi_schedule_connect_task(0);
                s_wifi_ble_fast_retry_at_ms = now_ms + APP_WIFI_BLE_FAST_RETRY_MS;
            }
        }
        if (handle_failure) {
            app_wifi_handle_startup_sta_failure("connect_timeout");
        } else if (schedule_connect_now) {
            app_wifi_schedule_connect_task(0);
            s_wifi_startup_retry_at_ms = now_ms + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void app_wifi_auto_connect_suspend(void)
{
    s_wifi_auto_connect_suspended = true;
}

static void app_wifi_auto_connect_resume(void)
{
    s_wifi_auto_connect_suspended = false;
}

static bool app_wifi_wait_connect_task_idle(uint32_t timeout_ms)
{
    int64_t deadline = app_wifi_now_ms() + (int64_t)timeout_ms;

    while (s_wifi_connect_task != NULL && app_wifi_now_ms() < deadline) {
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return s_wifi_connect_task == NULL;
}

/* 返回 true 表示调度请求已提交(任务创建成功);false=被门控/已存在/创建失败。
 * 首次调度失败不影响 apply 完成语义:窗口已建立,monitor 1s 内重试。 */
static bool app_wifi_schedule_connect_task(TickType_t delay_ticks)
{
    uint8_t net_mode;
    if (s_wifi_auto_connect_suspended) {
        return false;
    }
    /* M1 R2/R4:离线门控在调度入口拒绝;已排队任务由 task_fn 延时后二次检查拦截 */
    if (app_wifi_connect_blocked()) {
        return false;
    }
    app_config_lock();
    net_mode = s_config.net_mode;
    app_config_unlock();
    if (s_wifi_connect_task != NULL || !app_network_mode_has_sta(net_mode)) {
        return false;
    }
    if (xTaskCreate(app_wifi_connect_task_fn, "wifi_auto_connect", 4096,
                    (void *)(uintptr_t)delay_ticks, 5, &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = NULL;
        ESP_LOGE(TAG, "Failed to create Wi-Fi auto-connect task");
        return false;
    }
    return true;
}

/* STA 断连原因码 → 可读名(问题与版本优化.md 1.3-08)。
 * 覆盖外场最常见的失败:NO_AP_FOUND / 4WAY_HANDSHAKE_TIMEOUT(=口令错) / AUTH_FAIL 等。 */
static const char *app_wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY";
    case 6:   return "NOT_AUTHED";
    case 7:   return "NOT_ASSOCED";
    case 8:   return "ASSOC_LEAVE";
    case 9:   return "ASSOC_NOT_AUTHED";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:  return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:  return "IE_IN_4WAY_DIFFERS";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    case 208: return "ASSOC_COMEBACK_TIMEOUT";
    case 209: return "SA_QUERY_TIMEOUT";
    case 210: return "NO_AP_FOUND_W_COMPAT_SECURITY";   /* 1.3-04 实测命中:安全模式与阈值不兼容 */
    case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case 212: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:  return "OTHER";
    }
}

/* 扫描所见的 AP 安全模式 → 可读名(1.3-04:让"为什么不兼容"一眼可读) */
static const char *app_wifi_auth_name(uint8_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "OTHER";
    }
}

/* 打印"我们实际在要求什么"——排查 210 时与扫描到的 auth 直接对照(1.3-21) */
static void app_wifi_log_sta_cfg(const char *where, const wifi_config_t *cfg)
{
    ESP_LOGI(TAG, "%s: ssid='%s' pwd_len=%u threshold=%u(%s) pmf(capable=%d,required=%d) scan=%u",
             where,
             (const char *)cfg->sta.ssid,
             (unsigned)strlen((const char *)cfg->sta.password),
             (unsigned)cfg->sta.threshold.authmode,
             app_wifi_auth_name((uint8_t)cfg->sta.threshold.authmode),
             (int)cfg->sta.pmf_cfg.capable,
             (int)cfg->sta.pmf_cfg.required,
             (unsigned)cfg->sta.scan_method);
}

/* 210 兜底:AP 要求 PMF(802.11w)时首轮 required=false 会连不上,带 required=true 再试一次 */
static esp_err_t app_wifi_retry_with_pmf_required(void)
{
    wifi_config_t sta_cfg = {0};
    esp_err_t err;

    app_config_lock();
    app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), s_config.sta_ssid);
    app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), s_config.sta_password);
    app_config_unlock();

    if (sta_cfg.sta.ssid[0] == '\0' || sta_cfg.sta.password[0] == '\0') {
        ESP_LOGW(TAG, "PMF retry skipped: empty sta_ssid/sta_password in config");
        return ESP_ERR_INVALID_STATE;
    }
    sta_cfg.sta.threshold.authmode =
        (strlen((const char *)sta_cfg.sta.password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = true;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    /* M1-A9:许可在 set_config 之前取得,覆盖配置写入与连接 */
    if (!app_wifi_connect_begin("pmf_retry")) {
        return ESP_ERR_INVALID_STATE;
    }
    app_wifi_log_sta_cfg("pmf_required_retry", &sta_cfg);
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMF retry set_config failed: %s", esp_err_to_name(err));
        app_wifi_connect_end("pmf_retry");
        return err;
    }
    err = esp_wifi_connect();
    app_wifi_connect_end("pmf_retry");
    return err;
}

static app_wifi_try_match_t app_wifi_tryconnect_match_locked(const uint8_t *ssid,
                                                            uint8_t ssid_len,
                                                            const uint8_t *bssid)
{
    bool have_identity = false;
    bool ssid_match = false;
    bool bssid_match = false;
    bool bssid_nonzero = false;
    bool bssid_conflict = false;
    size_t i;

    if (ssid != NULL && ssid_len > 0) {
        have_identity = true;
        if (ssid_len == s_tryconnect_candidate_ssid_len &&
            memcmp(ssid, s_tryconnect_candidate_ssid, ssid_len) == 0) {
            ssid_match = true;
        }
    }
    if (bssid != NULL) {
        for (i = 0; i < 6; i++) {
            if (bssid[i] != 0) {
                bssid_nonzero = true;
                break;
            }
        }
        if (bssid_nonzero) {
            have_identity = true;
            if (s_tryconnect_candidate_bssid_valid) {
                if (memcmp(bssid, s_tryconnect_candidate_bssid, 6) == 0) {
                    bssid_match = true;
                } else {
                    bssid_conflict = true;
                }
            }
        }
    }

    /* BSSID 是更强的归属证据：同一 SSID 的不同 BSSID 不能因 SSID 相同
     * 就被认领为候选；信息冲突时按 UNKNOWN 处理，不终结候选。 */
    if (bssid_conflict) {
        return ssid_match ? APP_WIFI_TRY_MATCH_UNKNOWN : APP_WIFI_TRY_MATCH_OLD;
    }
    if (ssid_match || bssid_match) {
        return APP_WIFI_TRY_MATCH_CANDIDATE;
    }
    if (have_identity) {
        return APP_WIFI_TRY_MATCH_OLD;
    }
    return APP_WIFI_TRY_MATCH_UNKNOWN;
}

static app_wifi_try_result_t app_wifi_tryconnect_classify_reason(uint8_t reason)
{
    switch (reason) {
    case 15:  /* 4WAY_HANDSHAKE_TIMEOUT */
    case 202: /* AUTH_FAIL */
    case 2:   /* AUTH_EXPIRE */
        return APP_WIFI_TRY_WRONG_PASSWORD;
    case 201: /* NO_AP_FOUND */
    case 203: /* ASSOC_FAIL */
        return APP_WIFI_TRY_AP_NOT_FOUND;
    default:
        return APP_WIFI_TRY_CONNECT_FAILED;
    }
}

/* 返回 true 表示该事件已由 tryconnect 生命周期消费，普通重连逻辑必须跳过。 */
static bool app_wifi_tryconnect_on_disconnected(const wifi_event_sta_disconnected_t *event)
{
    bool handled = false;
    bool old_event = false;
    bool done_event = false;
    bool unknown_event = false;
    app_wifi_try_phase_t phase;
    app_wifi_try_result_t result = APP_WIFI_TRY_CONNECT_FAILED;
    EventGroupHandle_t events;

    app_wifi_tryconnect_state_lock();
    phase = s_tryconnect_phase;
    events = s_tryconnect_events;
    if (s_tryconnect_active && phase != APP_WIFI_TRY_PHASE_IDLE) {
        handled = true;
        switch (phase) {
        case APP_WIFI_TRY_PHASE_PRE_SCAN:
        case APP_WIFI_TRY_PHASE_CLEANUP_OLD:
        case APP_WIFI_TRY_PHASE_WAIT_OLD_DISCONNECT:
            /* 候选连接尚未启动：所有断连都只能属于旧生命周期。 */
            old_event = true;
            break;
        case APP_WIFI_TRY_PHASE_CANDIDATE_STARTING:
            /* 候选配置/启动窗口：不接受成功，也不接受失败。 */
            break;
        case APP_WIFI_TRY_PHASE_CANDIDATE_CONNECTING: {
            app_wifi_try_match_t match = app_wifi_tryconnect_match_locked(
                event->ssid, event->ssid_len, event->bssid);
            if (match == APP_WIFI_TRY_MATCH_CANDIDATE) {
                result = app_wifi_tryconnect_classify_reason(event->reason);
                s_tryconnect_result = result;
                s_tryconnect_phase = APP_WIFI_TRY_PHASE_DONE;
                done_event = true;
            } else if (match == APP_WIFI_TRY_MATCH_UNKNOWN) {
                unknown_event = true;
            }
            /* 旧 AP/信息不足的事件不写候选结果；候选失败/超时仍由
             * 真实候选事件或上层 timeout 收敛。 */
            break;
        }
        case APP_WIFI_TRY_PHASE_DONE:
            break;
        default:
            break;
        }
    }
    app_wifi_tryconnect_state_unlock();

    if (old_event) {
        ESP_LOGI(TAG, "tryconnect: old disconnect consumed reason=%d(%s)",
                 event->reason, app_wifi_reason_name(event->reason));
    }
    if (done_event) {
        ESP_LOGI(TAG, "tryconnect failed: reason=%d(%s)",
                 event->reason, app_wifi_reason_name(event->reason));
    } else if (unknown_event) {
        ESP_LOGW(TAG, "tryconnect: unowned disconnect ignored reason=%d(%s)",
                 event->reason, app_wifi_reason_name(event->reason));
    }
    if (events != NULL && old_event) {
        xEventGroupSetBits(events, APP_WIFI_TRY_OLD_DISC_BIT);
    }
    if (events != NULL && done_event) {
        xEventGroupSetBits(events, APP_WIFI_TRY_DONE_BIT);
    }
    return handled;
}

static bool app_wifi_tryconnect_on_connected(const wifi_event_sta_connected_t *event)
{
    bool handled = false;
    bool done_event = false;
    bool need_dhcp = false;
    EventGroupHandle_t events;

    app_wifi_tryconnect_state_lock();
    events = s_tryconnect_events;
    if (s_tryconnect_active && s_tryconnect_phase != APP_WIFI_TRY_PHASE_IDLE) {
        handled = true;
        if (s_tryconnect_phase == APP_WIFI_TRY_PHASE_CANDIDATE_CONNECTING &&
            app_wifi_tryconnect_match_locked(event->ssid, event->ssid_len,
                                             event->bssid) == APP_WIFI_TRY_MATCH_CANDIDATE) {
            if (!s_tryconnect_require_ip) {
                s_tryconnect_result = APP_WIFI_TRY_OK;
                s_tryconnect_phase = APP_WIFI_TRY_PHASE_DONE;
                done_event = true;
            } else {
                need_dhcp = true;
            }
        }
    }
    app_wifi_tryconnect_state_unlock();

    if (need_dhcp) {
        esp_err_t stop_err = esp_netif_dhcpc_stop(s_sta_netif);
        esp_err_t start_err;

        if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
            ESP_LOGW(TAG, "tryconnect: dhcpc_stop: %s", esp_err_to_name(stop_err));
        }
        start_err = esp_netif_dhcpc_start(s_sta_netif);
        if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGW(TAG, "tryconnect: dhcpc_start: %s", esp_err_to_name(start_err));
        }
    }
    if (events != NULL && done_event) {
        xEventGroupSetBits(events, APP_WIFI_TRY_DONE_BIT);
    }
    return handled;
}

static bool app_wifi_tryconnect_on_got_ip(void)
{
    bool handled = false;
    bool candidate_wait = false;
    bool done_event = false;
    EventGroupHandle_t events;
    wifi_ap_record_t ap_info;

    app_wifi_tryconnect_state_lock();
    events = s_tryconnect_events;
    if (s_tryconnect_active && s_tryconnect_phase != APP_WIFI_TRY_PHASE_IDLE) {
        handled = true;
        candidate_wait = (s_tryconnect_phase == APP_WIFI_TRY_PHASE_CANDIDATE_CONNECTING &&
                          s_tryconnect_require_ip);
    }
    app_wifi_tryconnect_state_unlock();

    if (candidate_wait && esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
        size_t ssid_len = strlen((const char *)ap_info.ssid);

        app_wifi_tryconnect_state_lock();
        if (s_tryconnect_active &&
            s_tryconnect_phase == APP_WIFI_TRY_PHASE_CANDIDATE_CONNECTING &&
            s_tryconnect_require_ip &&
            app_wifi_tryconnect_match_locked(ap_info.ssid, (uint8_t)ssid_len,
                                             ap_info.bssid) == APP_WIFI_TRY_MATCH_CANDIDATE) {
            s_tryconnect_result = APP_WIFI_TRY_OK;
            s_tryconnect_phase = APP_WIFI_TRY_PHASE_DONE;
            done_event = true;
        }
        app_wifi_tryconnect_state_unlock();
    }
    /* 查询失败/归属不明：不判成功也不判失败，继续等待有效事件或 timeout。 */
    if (events != NULL && done_event) {
        xEventGroupSetBits(events, APP_WIFI_TRY_DONE_BIT);
    }
    return handled;
}
static void app_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START: {
            bool await_start = false;
            uint32_t await_gen = 0;

            app_wifi_profiles_lock();
            s_wifi_attempted_mask = 0;
            app_wifi_profiles_unlock();

            /* A8:本次 apply 等待的 START —— 释放门控/hold 并开窗口,
             * 释放与窗口逻辑同点执行,避免事件先于释放被 R5 吞掉。 */
            app_wifi_runtime_lock();
            await_start = s_wifi_apply_await_start;
            await_gen = s_wifi_apply_start_gen;
            if (await_start) {
                /* 消费与完成标记同一临界区:超时侧不会看到"已消费未完成" */
                s_wifi_apply_await_start = false;
                s_wifi_apply_start_consumed_gen = await_gen;
            }
            app_wifi_runtime_unlock();
            if (await_start) {
                bool sched_ok;
                ESP_LOGI(TAG, "apply_start_released gen=%u", (unsigned)await_gen);
                /* 顺序(A8-r10):先建立启动窗口状态(此时门控仍关闭,任何
                 * 已排队任务都无法取得许可),再释放 gate/hold,最后调度;
                 * 保证许可释放后不会出现"窗口尚未就绪"的连接。 */
                app_wifi_start_startup_connect_window();
                app_wifi_offline_gate_clear_if_set();
                app_wifi_apply_hold_clear();
                sched_ok = app_wifi_schedule_connect_task(0);
                if (!sched_ok) {
                    /* 窗口已建立;monitor 会在 1s 内重试首次调度 */
                    ESP_LOGW(TAG, "first schedule deferred to monitor (gen=%u)",
                             (unsigned)await_gen);
                }
                /* 完成标记 = 窗口已建立 + 首次调度已尝试;在通知之前置位,
                 * 保证等待方看到 bit 时 completed_gen 已可见。 */
                app_wifi_runtime_lock();
                s_wifi_apply_start_completed_gen = await_gen;
                app_wifi_runtime_unlock();
                if (s_wifi_start_events != NULL) {
                    xEventGroupSetBits(s_wifi_start_events, APP_WIFI_START_EVENT_BIT);
                }
#ifdef APP_WIFI_FAULT_INJECT
                {
                    bool hook_active;
                    uint32_t hook_consumed;
                    uint32_t hook_completed;

                    portENTER_CRITICAL(&s_wifi_hook_mux);
                    hook_active = s_wifi_hook.active;
                    portEXIT_CRITICAL(&s_wifi_hook_mux);
                    app_wifi_runtime_lock();
                    hook_consumed = s_wifi_apply_start_consumed_gen;
                    hook_completed = s_wifi_apply_start_completed_gen;
                    app_wifi_runtime_unlock();
                    if (hook_active) {
                        ESP_LOGI(TAG, "hook_sta_start t=%lldms gen=%u consumed=%u completed=%u",
                                 (long long)app_wifi_now_ms(), (unsigned)await_gen,
                                 (unsigned)hook_consumed, (unsigned)hook_completed);
                    }
                }
#endif
                break;
            }

            /* M1 R5:离线门控下 STA_START 只记录,不开 90s 窗口、不调度连接
             * (受控恢复走 apply 链,由 offline_gate_clear 显式放行)。 */
            if (app_wifi_connect_blocked()) {
                ESP_LOGI(TAG, "offline gate: STA_START ignored");
                break;
            }
            app_wifi_start_startup_connect_window();
            app_wifi_schedule_connect_task(0);
            break;
        }
        case WIFI_EVENT_STA_CONNECTED: {
            bool use_static = false;
            wifi_ap_record_t ap_info;

            /* M1 R5:离线门控下迟到的关联事件只记录,不改运行态、不动 DHCP。 */
            if (app_wifi_connect_blocked()) {
                ESP_LOGI(TAG, "offline gate: STA_CONNECTED ignored");
                break;
            }

            /* 运行态 SSID 以实际关联结果为准：试连与自动回退都能如实反映 */
            if (esp_wifi_sta_get_ap_info(&ap_info) == ESP_OK) {
                app_wifi_set_connected_ssid((const char *)ap_info.ssid);
            }

            /* 试连验证:只有明确匹配当前候选 SSID/BSSID 的事件才可改结果。 */
            if (app_wifi_tryconnect_on_connected(
                    (const wifi_event_sta_connected_t *)event_data)) {
                break;
            }

            app_wifi_runtime_lock();
            s_wifi.sta_connected = true;
            s_wifi.sta_retries = 0;
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = false;
            s_wifi_startup_retry_at_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
            s_sta_connected_at_ms = app_wifi_now_ms();

            app_config_lock();
            use_static = s_config.sta_use_static;
            app_config_unlock();

            if (use_static) {
                /* 静态 IP 已在 apply_wifi_config 时写入 netif,连接后无需再动 DHCP;
                 * IP_EVENT_STA_GOT_IP 仍会用该 IP 触发。 */
                ESP_LOGI(TAG, "STA_CONNECTED: static ip mode (connected at t=%lldms)",
                         (long long)s_sta_connected_at_ms);
            } else {
                esp_err_t stop_err = esp_netif_dhcpc_stop(s_sta_netif);
                if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                    ESP_LOGW(TAG, "STA_CONNECTED: dhcpc_stop: %s", esp_err_to_name(stop_err));
                }
                esp_err_t start_err = esp_netif_dhcpc_start(s_sta_netif);
                if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                    ESP_LOGW(TAG, "STA_CONNECTED: dhcpc_start: %s", esp_err_to_name(start_err));
                }
                ESP_LOGI(TAG, "STA_CONNECTED: dhcpc restarted (connected at t=%lldms)",
                         (long long)s_sta_connected_at_ms);
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = event_data;
            uint8_t sta_retries_snapshot;
            int active_profile_snapshot;
            uint8_t net_mode_snapshot;
            bool need_schedule_connect = false;
            bool do_retry_direct = false;

            /* 链路已断，运行态 SSID 立即失效。保留最后连过的值会误导 App 判"还连着"。 */
            app_wifi_set_connected_ssid("");

            /* 试连验证:旧链路断开不能终结候选;只有候选 SSID/BSSID
             * 明确匹配的事件才可写结果。事件归属由 helper 统一处理。 */
            if (app_wifi_tryconnect_on_disconnected(disconnected)) {
                break;
            }

            app_wifi_runtime_lock();
            s_wifi.sta_connected = false;
            s_wifi.sta_has_ip = false;
            s_wifi.sta_ip_conflict = false;
            sta_retries_snapshot = s_wifi.sta_retries;
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = false;
            app_set_sta_ip_strings(NULL);
            app_wifi_runtime_lock();
            snprintf(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "%d(%s)",
                     disconnected->reason, app_wifi_reason_name(disconnected->reason));
            app_wifi_runtime_unlock();

            app_config_lock();
            net_mode_snapshot = s_config.net_mode;
            app_config_unlock();
            app_wifi_profiles_lock();
            active_profile_snapshot = s_wifi_active_profile_index;
            app_wifi_profiles_unlock();

            /* 外场可诊断(问题与版本优化.md 1.3-08):断连原因 + 重试计数必须可见。
             * 旧实现只写 last_disconnect 字符串、**不打任何日志**,现场与"设备卡死"无法区分。 */
            ESP_LOGW(TAG, "STA disconnected: reason=%d(%s) retries=%u has_sta=%d profile=%d",
                     disconnected->reason, app_wifi_reason_name(disconnected->reason),
                     (unsigned)sta_retries_snapshot,
                     (int)app_network_mode_has_sta(net_mode_snapshot),
                     active_profile_snapshot);

            /* M1 R5:迟到的断开事件在离线态只保留上面的状态卫生,
             * 之后任何重试动作(PMF 重试/直连/调度)一律禁止。 */
            if (app_wifi_connect_blocked()) {
                ESP_LOGI(TAG, "offline gate: disconnect event retry suppressed");
                break;
            }

            /* 1.3-21:210 在"AP 要求 PMF(802.11w)"时也会出现 —— 首轮 required=false 失败后,
             * 带 required=true 再试一次(每次启动只试一次,避免反复翻转)。 */
            if (disconnected->reason == 210 && !s_wifi_pmf_required_tried) {
                s_wifi_pmf_required_tried = true;
                app_wifi_runtime_lock();
                s_wifi.sta_retries = 0;
                app_wifi_runtime_unlock();
                s_wifi_sta_connecting = true;
                ESP_LOGW(TAG, "reason=210: retry once with PMF required (802.11w)");
                if (app_wifi_retry_with_pmf_required() != ESP_OK) {
                    s_wifi_sta_connecting = false;
                    ESP_LOGW(TAG, "PMF-required retry could not start");
                }
                break;
            }

            if (app_ble_gatts_connected()) {
                /* 产品决策 A:BLE 连接期间不跑事件链的 3 次直连/扫描 ——
                 * fast path 会把 sta_retries 重置为 0,AP 仍离线时形成
                 * "3 直连→formal-only→再 3 直连"的快速循环,绕过 120s。
                 * 这里只落状态,交给 monitor 的 120s formal-only 分支。 */
                ESP_LOGI(TAG, "STA down with BLE connected: event-chain retry suppressed; "
                              "120s formal-only monitor owns recovery");
                break;
            }

            if (app_network_mode_has_sta(net_mode_snapshot) &&
                active_profile_snapshot >= 0 &&
                sta_retries_snapshot < APP_WIFI_STA_RETRY_LIMIT) {
                app_wifi_runtime_lock();
                s_wifi.sta_retries = sta_retries_snapshot + 1U;
                app_wifi_runtime_unlock();
                s_wifi_sta_connecting = true;
                do_retry_direct = true;
            } else if (app_network_mode_has_sta(net_mode_snapshot)) {
                if (active_profile_snapshot >= 0 &&
                    (unsigned)active_profile_snapshot < APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS) {
                    app_wifi_profiles_lock();
                    s_wifi_attempted_mask |= (uint8_t)(1U << active_profile_snapshot);
                    app_wifi_profiles_unlock();
                }
                need_schedule_connect = true;
            }
            if (do_retry_direct) {
                /* M1 调用点 4/5(R3.2/R5):事件链直连绕过 schedule 入口,
                 * 必须纳入原子许可 —— 门控时整支跳过,不进入 begin。 */
                if (app_wifi_connect_begin("retry_direct")) {
                    esp_wifi_connect();
                    app_wifi_connect_end("retry_direct");
                }
            } else if (need_schedule_connect) {
                if (app_ble_gatts_connected()) {
                    /* BLE 连接期间不扫描 profile:只给一次正式 SSID
                     * fast path 机会,失败后交给 120s 降级重试。 */
                    s_wifi_fast_path_pending = true;
                    s_wifi_formal_only_pending = true;
                }
                app_wifi_schedule_connect_task(pdMS_TO_TICKS(250));
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;

        /* 试连验证:拿 IP 属候选连接,不写运行时状态/不回写 profile。
         * GOT_IP 本身不带 SSID，必须用当前 AP info 校验归属；查询失败
         * 时继续等待有效事件/超时，不能误报成功或失败。 */
        if (app_wifi_tryconnect_on_got_ip()) {
            return;
        }

        /* M1 R5:离线门控下迟到的 GOT_IP 只记录,不写运行态、不启动冲突检测。 */
        if (app_wifi_connect_blocked()) {
            ESP_LOGI(TAG, "offline gate: GOT_IP ignored");
            return;
        }

        app_wifi_runtime_lock();
        s_wifi.sta_connected = true;
        s_wifi.sta_has_ip = true;
        s_wifi.sta_retries = 0;
        app_wifi_runtime_unlock();
        s_wifi_sta_connecting = false;
        app_wifi_profiles_lock();
        s_wifi_attempted_mask = 0;
        app_wifi_profiles_unlock();
        /* 拿到 IP:运行时兜底退避归零,下次断网从 5s 重新开始 */
        s_wifi_rt_retry_at_ms = 0;
        s_wifi_rt_retry_step = 0;
        s_wifi_rt_retry_cycle = 0;
        s_wifi_ble_fast_retry_at_ms = 0;
        s_wifi_ble_fast_cycle = 0;
        app_set_sta_ip_strings(&event->ip_info);
        app_wifi_stop_startup_connect_window();
        app_wifi_set_startup_fallback_reason("none");
        ESP_LOGI(TAG,
                 "IP_EVENT_STA_GOT_IP: %s (t=%lldms from boot, %lldms from connected)",
                 s_wifi.sta_ip, (long long)app_wifi_now_ms(),
                 s_sta_connected_at_ms > 0 ? (long long)(app_wifi_now_ms() - s_sta_connected_at_ms) : 0LL);

        /* 静态 IP 模式:拿到地址后做一次 ARP 冲突探测(见 app_sta_ip_conflict_check_task) */
        {
            bool use_static = false;

            app_config_lock();
            use_static = s_config.sta_use_static;
            app_config_unlock();
            if (use_static) {
                app_sta_ip_conflict_check_start();
            }
        }

        /* P0-4: 正式配置只允许由 network.apply / config.set 修改。
         * 这里原先会把自动重连选中的旧 profile（如 CMCC-502）只写
         * ssid/password 到 s_config，留下新配置的静态字段，造成 SSID
         * 与静态 IP 跨网络拼接。自动重连/profile 只是运行时回退，
         * 不得改写正式配置。 */
    }
}

/* ===== M1 启动安全:apply 拆分 / 失败记录 / LKG / 恢复链 / 离线门控 =====
 * 设计依据:docs/静态IP_M1启动安全设计稿_2026-11-03.md(冻结版)
 * 日志行为规格化的 §3.1 清单约束,发射点不得增删改格式(T15/T16 断言依据)。 */

static const char *app_wifi_apply_stage_name(app_wifi_apply_stage_t stage)
{
    switch (stage) {
    case APP_WIFI_APPLY_STAGE_WIFI_STOP:          return "wifi_stop";
    case APP_WIFI_APPLY_STAGE_SET_MODE:           return "set_mode";
    case APP_WIFI_APPLY_STAGE_SET_CONFIG:         return "set_config";
    case APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP:  return "static_dhcpc_stop";
    case APP_WIFI_APPLY_STAGE_STATIC_PARSE:       return "static_parse";
    case APP_WIFI_APPLY_STAGE_STATIC_SET_IP:      return "static_set_ip";
    case APP_WIFI_APPLY_STAGE_STATIC_SET_DNS:     return "static_set_dns";
    case APP_WIFI_APPLY_STAGE_DHCPC_START:        return "dhcpc_start";
    case APP_WIFI_APPLY_STAGE_WIFI_START:         return "wifi_start";
    default:                                      return "none";
    }
}

static const char *app_wifi_cfg_state_name(app_net_cfg_state_t state)
{
    switch (state) {
    case APP_NET_CFG_STATE_DHCP:                  return "dhcp";
    case APP_NET_CFG_STATE_STATIC:                return "static";
    case APP_NET_CFG_STATE_FALLBACK:              return "fallback";
    case APP_NET_CFG_STATE_OFFLINE:               return "offline_pending_config";
    case APP_NET_CFG_STATE_OFFLINE_STOP_FAILED:   return "offline_stop_failed";
    case APP_NET_CFG_STATE_OFFLINE_QUIESCE_TIMEOUT: return "offline_quiesce_timeout";
    default:                                      return "unknown";
    }
}

static void app_wifi_set_cfg_state(app_net_cfg_state_t state)
{
    app_wifi_runtime_lock();
    s_wifi.cfg_state = state;
    app_wifi_runtime_unlock();
    ESP_LOGI(TAG, "net_config_state=%s", app_wifi_cfg_state_name(state));
}

static void app_wifi_record_apply_failure(app_wifi_apply_stage_t stage, esp_err_t err,
                                          bool static_mode)
{
    app_wifi_runtime_lock();
    if (!s_wifi.apply_failure.valid) {
        /* 首个失败=根因,后续恢复失败不覆盖(设计稿 §3.1) */
        s_wifi.apply_failure.valid = true;
        s_wifi.apply_failure.stage = stage;
        s_wifi.apply_failure.err = err;
        s_wifi.apply_failure.static_mode = static_mode;
    }
    /* attempts 在 app_wifi_apply_cfg 入口计数(候选/LKG/救援各计 1,成功也计) */
    app_wifi_runtime_unlock();
    ESP_LOGE(TAG, "apply_failed stage=%s err=%s mode=%s attempt=%u",
             app_wifi_apply_stage_name(stage), esp_err_to_name(err),
             static_mode ? "static" : "dhcp",
             (unsigned)s_wifi.apply_failure.attempts);
}

static void app_wifi_build_cfg_from_config(app_wifi_apply_cfg_t *out)
{
    memset(out, 0, sizeof(*out));
    app_config_lock();
    out->sta_use_static = (s_config.sta_use_static != 0);
    app_copy_string(out->ssid, sizeof(out->ssid), s_config.sta_ssid);
    app_copy_string(out->password, sizeof(out->password), s_config.sta_password);
    app_copy_string(out->static_ip, sizeof(out->static_ip), s_config.sta_static_ip);
    app_copy_string(out->static_gw, sizeof(out->static_gw), s_config.sta_static_gateway);
    app_copy_string(out->static_mask, sizeof(out->static_mask), s_config.sta_static_netmask);
    app_copy_string(out->static_dns1, sizeof(out->static_dns1), s_config.sta_static_dns1);
    app_copy_string(out->static_dns2, sizeof(out->static_dns2), s_config.sta_static_dns2);
    app_config_unlock();
    out->valid = true;
}

/* LKG:只接受"成功应用"的快照(设计稿 §3.5 生命周期表);
 * 候选失败绝不 capture,config.set 仅保存(reboot 生效)也不经过这里。 */
static void app_wifi_lkg_capture(const app_wifi_apply_cfg_t *cfg)
{
    if (cfg == NULL || !cfg->valid) {
        return;
    }
    app_wifi_runtime_lock();
    s_wifi.lkg = *cfg;
    app_wifi_runtime_unlock();
}

static bool app_wifi_lkg_equals(const app_wifi_apply_cfg_t *cfg)
{
    bool equals;

    if (cfg == NULL) {
        return false;
    }
    app_wifi_runtime_lock();
    equals = s_wifi.lkg.valid &&
             s_wifi.lkg.sta_use_static == cfg->sta_use_static &&
             strcmp(s_wifi.lkg.ssid, cfg->ssid) == 0 &&
             strcmp(s_wifi.lkg.password, cfg->password) == 0 &&
             strcmp(s_wifi.lkg.static_ip, cfg->static_ip) == 0 &&
             strcmp(s_wifi.lkg.static_gw, cfg->static_gw) == 0 &&
             strcmp(s_wifi.lkg.static_mask, cfg->static_mask) == 0 &&
             strcmp(s_wifi.lkg.static_dns1, cfg->static_dns1) == 0 &&
             strcmp(s_wifi.lkg.static_dns2, cfg->static_dns2) == 0;
    app_wifi_runtime_unlock();
    return equals;
}

/* 原子连接许可(R3/R3.2):同一临界区内"检查离线 + inflight++"。
 * 返回 true 的调用方必须在所有路径调用 app_wifi_connect_end()。 */
static bool app_wifi_connect_blocked(void)
{
    bool blocked;

    app_wifi_runtime_lock();
    blocked = s_wifi.cfg_offline;
    app_wifi_runtime_unlock();
    return blocked;
}

/* A8:R9 入口拒绝条件 = 离线门控 || apply 危险段 hold || 等待启动事件。
 * 仅用于给出更准确的错误码;真正的互斥由 s_wifi_op_owner 保证。 */
static bool app_wifi_tryconnect_entry_blocked(void)
{
    bool blocked;

    app_wifi_runtime_lock();
    blocked = s_wifi.cfg_offline || s_wifi_apply_hold || s_wifi_apply_await_start;
    app_wifi_runtime_unlock();
    return blocked;
}

/* in-flight 上限防御:正常并发远低于此值;触顶说明配对异常,拒绝并显式报错 */
#define APP_WIFI_CONNECT_INFLIGHT_MAX 16

/* A7/A8 原语声明集中在文件头(见 s_wifi_apply_hold 定义) */

static bool app_wifi_connect_begin(const char *path)
{
    bool allowed = false;
    bool over_limit = false;
    uint32_t inflight = 0;

    app_wifi_runtime_lock();
    if (!s_wifi.cfg_offline && !s_wifi_apply_hold) {   /* A7:危险段 hold 同样拒绝新许可 */
        if (s_wifi.connect_inflight >= APP_WIFI_CONNECT_INFLIGHT_MAX) {
            over_limit = true;
            inflight = s_wifi.connect_inflight;
        } else {
            s_wifi.connect_inflight += 1;
            inflight = s_wifi.connect_inflight;
            allowed = true;
        }
    }
    app_wifi_runtime_unlock();
    if (allowed) {
        ESP_LOGD(TAG, "connect_begin inflight=%u path=%s", (unsigned)inflight, path);
#ifdef APP_WIFI_FAULT_INJECT
        app_wifi_test_connect_hold_wait(path);
#endif
    } else if (over_limit) {
        /* 配对异常信号:不得静默忽略 */
        ESP_LOGE(TAG, "connect_begin_refused inflight=%u path=%s", (unsigned)inflight, path);
    }
    return allowed;
}

static void app_wifi_connect_end(const char *path)
{
    uint32_t inflight;
    bool unpaired = false;

    app_wifi_runtime_lock();
    if (s_wifi.connect_inflight > 0) {
        s_wifi.connect_inflight -= 1;
    } else {
        /* 多 end/漏 begin 的配对异常:显式报错,不静默吞掉 */
        unpaired = true;
    }
    inflight = s_wifi.connect_inflight;
    app_wifi_runtime_unlock();
    if (unpaired) {
        ESP_LOGE(TAG, "connect_end_unpaired inflight=%u path=%s", (unsigned)inflight, path);
    } else {
        ESP_LOGD(TAG, "connect_end inflight=%u path=%s", (unsigned)inflight, path);
    }
}

/* quiesce 等待(R3.1):200ms 告警,limit_ms 为总上限;未归零返回 false,
 * 调用方禁止执行 esp_wifi_stop()。recovery=true 用 recover_ 前缀日志,
 * false 用 offline_ 前缀日志。 */
static bool app_wifi_wait_connect_inflight(int64_t limit_ms, bool recovery)
{
    int64_t start_ms = app_wifi_now_ms();
    int64_t deadline_ms = start_ms + limit_ms;
    bool warned = false;

    for (;;) {
        app_wifi_runtime_lock();
        uint32_t inflight = s_wifi.connect_inflight;
        app_wifi_runtime_unlock();

        if (inflight == 0) {
            if (recovery) {
                ESP_LOGI(TAG, "recover_quiesce_done inflight=0 elapsed=%ums",
                         (unsigned)(app_wifi_now_ms() - start_ms));
            }
            return true;
        }
        int64_t elapsed = app_wifi_now_ms() - start_ms;
        if (!warned && elapsed >= APP_WIFI_QUIESCE_WARN_MS) {
            warned = true;
            if (recovery) {
                ESP_LOGI(TAG, "recover_quiesce_wait inflight=%u elapsed=%ums",
                         (unsigned)inflight, (unsigned)elapsed);
            } else {
                ESP_LOGW(TAG, "offline_quiesce_wait: inflight=%u elapsed=%ums",
                         (unsigned)inflight, (unsigned)elapsed);
            }
        }
        if (app_wifi_now_ms() >= deadline_ms) {
            /* 日志行按冻结规格 §3.1 逐字对齐(两条格式不同,不得合并) */
            if (recovery) {
                ESP_LOGE(TAG, "recover_quiesce_timeout inflight=%u, recovery rejected",
                         (unsigned)inflight);
            } else {
                ESP_LOGE(TAG, "offline_quiesce_timeout: inflight=%u elapsed=%ums, wifi_stop NOT called",
                         (unsigned)inflight, (unsigned)elapsed);
            }
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

static void app_wifi_apply_cfg_common_reset(void)
{
    app_wifi_runtime_lock();
    s_wifi.sta_connected = false;
    s_wifi.sta_has_ip = false;
    s_wifi.sta_ip_conflict = false;
    s_wifi.sta_retries = 0;
    app_wifi_runtime_unlock();
    s_wifi_sta_connecting = false;
    s_wifi_pmf_required_tried = false;
    app_wifi_profiles_lock();
    s_wifi_active_profile_index = -1;
    s_wifi_attempted_mask = 0;
    app_wifi_profiles_unlock();
    app_wifi_stop_startup_connect_window();
    app_wifi_set_startup_fallback_reason("none");
    app_set_sta_ip_strings(NULL);
}

/* 核心 apply(设计稿 §3.2):成功返回 ESP_OK 并记 *stage_out=NONE;
 * 失败返回错误码并记最后执行阶段(首个失败由 record 层固化)。 */
/* A8:apply 链所有权在链入口 claim、链出口 release(含恢复链全程),
 * 与 tryconnect 通过 s_wifi_op_owner 事务级互斥。claim 失败不是配置应用
 * 失败:调用方只记录并放弃本轮,不进入恢复链。 */
static bool app_wifi_op_claim_apply_once(void)
{
    bool ok = false;

    app_wifi_tryconnect_state_lock();
    if (s_wifi_op_owner == APP_WIFI_OP_NONE) {
        s_wifi_op_owner = APP_WIFI_OP_APPLY;
        ok = true;
    }
    app_wifi_tryconnect_state_unlock();
    return ok;
}

static bool app_wifi_op_claim_apply(uint32_t wait_ms)
{
    int64_t deadline = app_wifi_now_ms() + (int64_t)wait_ms;

    for (;;) {
        if (app_wifi_op_claim_apply_once()) {
            return true;
        }
        if (app_wifi_now_ms() >= deadline) {
            ESP_LOGW(TAG, "APPLY_BUSY_TRYX: apply abandoned after %ums", (unsigned)wait_ms);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(50));
    }
}

static void app_wifi_op_release_apply(void)
{
    app_wifi_tryconnect_state_lock();
    if (s_wifi_op_owner == APP_WIFI_OP_APPLY) {
        s_wifi_op_owner = APP_WIFI_OP_NONE;
    }
    app_wifi_tryconnect_state_unlock();
}

static void app_wifi_apply_hold_set(void)
{
    app_wifi_runtime_lock();
    s_wifi_apply_hold = true;
    app_wifi_runtime_unlock();
}

static void app_wifi_apply_hold_clear(void)
{
    app_wifi_runtime_lock();
    bool was = s_wifi_apply_hold;
    s_wifi_apply_hold = false;
    app_wifi_runtime_unlock();
    if (was) {
        ESP_LOGD(TAG, "apply_hold_clear");
    }
}

static void app_wifi_offline_gate_clear_if_set(void)
{
    app_wifi_runtime_lock();
    bool was = s_wifi.cfg_offline;
    s_wifi.cfg_offline = false;
    app_wifi_runtime_unlock();
    if (was) {
        ESP_LOGI(TAG, "offline_gate_clear");   /* §3.1 规格行(发射点见 A7) */
    }
}

/* apply 危险段前的 in-flight 排空;1000ms 上限,超时按"本轮放弃"处理。
 * D/W 级日志,不属 §3.1 规格行。 */
static bool app_wifi_apply_drain_inflight(void)
{
    int64_t deadline = app_wifi_now_ms() + APP_WIFI_QUIESCE_LIMIT_DEFAULT_MS;

    for (;;) {
        app_wifi_runtime_lock();
        uint32_t inflight = s_wifi.connect_inflight;
        app_wifi_runtime_unlock();
        if (inflight == 0) {
            return true;
        }
        if (app_wifi_now_ms() >= deadline) {
            ESP_LOGW(TAG, "apply_drain_timeout inflight=%u", (unsigned)inflight);
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
}

#ifdef APP_WIFI_FAULT_INJECT
/* ===== M1 测试构建专用故障注入(默认 OFF;与 APP_C1_TEST_HOOKS 独立) ===== */
static bool app_wifi_test_stage_from_name(const char *name, app_wifi_apply_stage_t *out)
{
    static const struct { const char *n; app_wifi_apply_stage_t s; } map[] = {
        {"wifi_stop", APP_WIFI_APPLY_STAGE_WIFI_STOP},
        {"set_mode", APP_WIFI_APPLY_STAGE_SET_MODE},
        {"set_config", APP_WIFI_APPLY_STAGE_SET_CONFIG},
        {"static_dhcpc_stop", APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP},
        {"static_parse", APP_WIFI_APPLY_STAGE_STATIC_PARSE},
        {"static_set_ip", APP_WIFI_APPLY_STAGE_STATIC_SET_IP},
        {"dhcpc_start", APP_WIFI_APPLY_STAGE_DHCPC_START},
        {"wifi_start", APP_WIFI_APPLY_STAGE_WIFI_START},
    };
    if (name == NULL) return false;
    for (size_t i = 0; i < sizeof(map)/sizeof(map[0]); i++) {
        if (strcmp(name, map[i].n) == 0) { *out = map[i].s; return true; }
    }
    return false;
}

static bool app_wifi_test_take_failure(app_wifi_apply_stage_t stage)
{
    bool hit = false;
    int remaining = 0;

    portENTER_CRITICAL(&s_wifi_hook_mux);
    if (s_wifi_hook.active && s_wifi_hook.fail_count > 0 &&
        s_wifi_hook.fail_stage == stage) {
        s_wifi_hook.fail_count -= 1;
        remaining = s_wifi_hook.fail_count;
        hit = true;
    }
    portEXIT_CRITICAL(&s_wifi_hook_mux);
    if (hit) {
        ESP_LOGW(TAG, "hook_apply_fail stage=%s remaining=%d",
                 app_wifi_apply_stage_name(stage), remaining);
    }
    return hit;
}

static bool app_wifi_test_take_stop_failure(void)
{
    bool hit = false;
    int remaining = 0;

    portENTER_CRITICAL(&s_wifi_hook_mux);
    if (s_wifi_hook.active && s_wifi_hook.stop_fail_count > 0) {
        s_wifi_hook.stop_fail_count -= 1;
        remaining = s_wifi_hook.stop_fail_count;
        hit = true;
    }
    portEXIT_CRITICAL(&s_wifi_hook_mux);
    if (hit) {
        ESP_LOGW(TAG, "hook_stop_fail remaining=%d", remaining);
    }
    return hit;
}

static void app_wifi_test_connect_hold_wait(const char *path)
{
    bool claimed = false;
    bool released = false;
    int timeout_ms = 0;
    uint32_t seq = 0;
    uint32_t my_gen = 0;
    int64_t deadline;

    /* 单次领取:只有第一个 connect 调用拿到本轮 hold;保存本轮 generation,
     * 退出时只有 generation 未变才允许清理状态,避免覆盖新一轮布防。 */
    portENTER_CRITICAL(&s_wifi_hook_mux);
    if (s_wifi_hook.active && s_wifi_hook.hold_armed && !s_wifi_hook.hold_claimed) {
        s_wifi_hook.hold_claimed = true;
        s_wifi_hook.hold_seq += 1;
        seq = s_wifi_hook.hold_seq;
        my_gen = s_wifi_hook.hold_gen;
        timeout_ms = s_wifi_hook.hold_timeout_ms;
        claimed = true;
    }
    portEXIT_CRITICAL(&s_wifi_hook_mux);
    if (!claimed) {
        return;
    }
    ESP_LOGW(TAG, "hook_connect_hold_begin path=%s seq=%u gen=%u timeout=%dms",
             path, (unsigned)seq, (unsigned)my_gen, timeout_ms);
    deadline = app_wifi_now_ms() + timeout_ms;
    for (;;) {
        bool superseded;
        portENTER_CRITICAL(&s_wifi_hook_mux);
        released = s_wifi_hook.hold_release;
        superseded = (s_wifi_hook.hold_gen != my_gen);
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        if (superseded) {
            ESP_LOGW(TAG, "hook_connect_hold_superseded gen=%u", (unsigned)my_gen);
            break;
        }
        if (released || app_wifi_now_ms() >= deadline) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));   /* 等待期间不持锁 */
    }
    portENTER_CRITICAL(&s_wifi_hook_mux);
    if (s_wifi_hook.hold_gen == my_gen) {   /* 只有本轮仍有效才清理 */
        s_wifi_hook.hold_claimed = false;
        s_wifi_hook.hold_armed = false;
        s_wifi_hook.hold_release = false;
    }
    portEXIT_CRITICAL(&s_wifi_hook_mux);
    if (released) {
        ESP_LOGW(TAG, "hook_connect_hold_end path=%s seq=%u (released)",
                 path, (unsigned)seq);
    } else {
        ESP_LOGE(TAG, "hook_connect_hold_timeout path=%s seq=%u",
                 path, (unsigned)seq);
    }
}

esp_err_t app_wifi_test_hook(const char *action, const char *stage,
                             int count, int timeout_ms)
{
    if (action == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (strcmp(action, "apply_fail") == 0) {
        app_wifi_apply_stage_t st;
        esp_err_t ret = ESP_OK;
        if (!app_wifi_test_stage_from_name(stage, &st) || count <= 0) {
            return ESP_ERR_INVALID_ARG;
        }
        portENTER_CRITICAL(&s_wifi_hook_mux);
        if (s_wifi_hook.fail_count > 0) {
            ret = ESP_ERR_INVALID_STATE;   /* 已有未消费注入(boot 票据/RAM):不覆盖 */
        } else {
            s_wifi_hook.active = true;
            s_wifi_hook.fail_stage = st;
            s_wifi_hook.fail_count = count;
        }
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "hook_apply_fail_busy");
            return ret;
        }
        ESP_LOGW(TAG, "hook_armed apply_fail stage=%s count=%d", stage, count);
        return ESP_OK;
    }
    if (strcmp(action, "boot_fail") == 0) {
        app_wifi_apply_stage_t st;
        if (!app_wifi_test_stage_from_name(stage, &st) || count != 1) {
            return ESP_ERR_INVALID_ARG;
        }
        return app_wifi_boot_ticket_write((uint16_t)st, 1);
    }
    if (strcmp(action, "rtc_probe") == 0) {
        return app_wifi_boot_ticket_write(APP_WIFI_BOOT_TICKET_PROBE_STAGE, 0);
    }
    if (strcmp(action, "stop_fail") == 0) {
        if (count <= 0) return ESP_ERR_INVALID_ARG;
        portENTER_CRITICAL(&s_wifi_hook_mux);
        s_wifi_hook.active = true;
        s_wifi_hook.stop_fail_count = count;
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        ESP_LOGW(TAG, "hook_armed stop_fail count=%d", count);
        return ESP_OK;
    }
    if (strcmp(action, "connect_hold") == 0) {
        esp_err_t ret = ESP_OK;
        if (timeout_ms > 10000) {
            return ESP_ERR_INVALID_ARG;   /* 上限 10s,防止误操作长期占用许可 */
        }
        portENTER_CRITICAL(&s_wifi_hook_mux);
        if (s_wifi_hook.hold_claimed) {
            ret = ESP_ERR_INVALID_STATE;  /* 上一轮 hold 尚未结束:拒绝覆盖 */
        } else {
            s_wifi_hook.active = true;
            s_wifi_hook.hold_armed = true;
            s_wifi_hook.hold_claimed = false;
            s_wifi_hook.hold_release = false;
            s_wifi_hook.hold_gen += 1;
            s_wifi_hook.hold_timeout_ms = (timeout_ms > 0) ? timeout_ms : 2000;
        }
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        if (ret != ESP_OK) {
            ESP_LOGW(TAG, "hook_connect_hold_busy");
            return ret;
        }
        ESP_LOGW(TAG, "hook_armed connect_hold timeout=%dms",
                 (timeout_ms > 0) ? timeout_ms : 2000);
        return ESP_OK;
    }
    if (strcmp(action, "connect_release") == 0) {
        portENTER_CRITICAL(&s_wifi_hook_mux);
        s_wifi_hook.hold_release = true;
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        ESP_LOGW(TAG, "hook_connect_release");
        return ESP_OK;
    }
    if (strcmp(action, "force_offline") == 0) {
        ESP_LOGW(TAG, "hook_force_offline");
        app_wifi_enter_offline();
        return ESP_OK;
    }
    if (strcmp(action, "status") == 0) {
        bool active;
        app_wifi_apply_stage_t st;
        int fail_count;
        int stop_count;
        bool hold_armed;
        bool await;
        uint32_t consumed;
        uint32_t completed;

        portENTER_CRITICAL(&s_wifi_hook_mux);
        active = s_wifi_hook.active;
        st = s_wifi_hook.fail_stage;
        fail_count = s_wifi_hook.fail_count;
        stop_count = s_wifi_hook.stop_fail_count;
        hold_armed = s_wifi_hook.hold_armed;
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        app_wifi_runtime_lock();
        await = s_wifi_apply_await_start;
        consumed = s_wifi_apply_start_consumed_gen;
        completed = s_wifi_apply_start_completed_gen;
        app_wifi_runtime_unlock();
        ESP_LOGI(TAG, "hook_status active=%d fail_stage=%s fail_count=%d stop_fail=%d "
                      "hold_armed=%d await=%d consumed=%u completed=%u",
                 (int)active, app_wifi_apply_stage_name(st), fail_count, stop_count,
                 (int)hold_armed, (int)await, (unsigned)consumed, (unsigned)completed);
        return ESP_OK;
    }
    if (strcmp(action, "clear") == 0) {
        portENTER_CRITICAL(&s_wifi_hook_mux);
        /* 先推进 generation 使旧等待者退出时不清理新状态;
         * 置 release 唤醒等待者;claimed 归零允许后续重新布防。 */
        s_wifi_hook.hold_gen += 1;
        s_wifi_hook.hold_release = true;
        s_wifi_hook.hold_armed = false;
        s_wifi_hook.hold_claimed = false;
        s_wifi_hook.active = false;
        s_wifi_hook.fail_count = 0;
        s_wifi_hook.stop_fail_count = 0;
        s_wifi_hook.hold_timeout_ms = 0;
        portEXIT_CRITICAL(&s_wifi_hook_mux);
        ESP_LOGW(TAG, "hook_cleared");
        return ESP_OK;
    }
    return ESP_ERR_INVALID_ARG;
}
#endif /* APP_WIFI_FAULT_INJECT */

static esp_err_t app_wifi_apply_cfg(const app_wifi_apply_cfg_t *cfg,
                                    app_wifi_apply_stage_t *stage_out)
{
    wifi_config_t sta_cfg = {0};
    esp_err_t err;
    app_wifi_apply_stage_t stage = APP_WIFI_APPLY_STAGE_NONE;

    /* A8:所有权由链入口 claim(调用方),这里只在本 apply 内封锁新许可并
     * 排空已获许可的自动连接调用;tryconnect 互斥由 OP_APPLY 保证。 */
    app_wifi_apply_hold_set();
    if (!app_wifi_apply_drain_inflight()) {
        app_wifi_apply_hold_clear();
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_NONE;
        }
        return ESP_ERR_INVALID_STATE;
    }

    if (cfg == NULL || !cfg->valid || s_sta_netif == NULL) {
        app_wifi_apply_hold_clear();
        if (stage_out != NULL) {
            *stage_out = APP_WIFI_APPLY_STAGE_SET_MODE;
        }
        return ESP_ERR_INVALID_STATE;
    }

    /* attempts 计数(§3.1/A5):参数有效、真正进入应用流程才计 1;
     * 候选/LKG/救援均计,成功也计 —— boot 正常=1,恢复链最多 3。 */
    app_wifi_runtime_lock();
    s_wifi.apply_failure.attempts += 1;
    app_wifi_runtime_unlock();

    app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), cfg->ssid);
    app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), cfg->password);
    sta_cfg.sta.threshold.authmode =
        (strlen((const char *)sta_cfg.sta.password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    app_wifi_log_sta_cfg("apply_cfg", &sta_cfg);
    ESP_LOGI(TAG, "apply_cfg: STA-only sta_pwd_len=%u static=%d",
             (unsigned)strlen((const char *)sta_cfg.sta.password),
             (int)cfg->sta_use_static);

    stage = APP_WIFI_APPLY_STAGE_WIFI_STOP;
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_WIFI_STOP)) {
        err = ESP_FAIL;
        goto out_fail;
    }
#endif
    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        ESP_LOGE(TAG, "apply_cfg: wifi_stop failed: %s", esp_err_to_name(err));
        goto out_fail;
    }

    app_wifi_apply_cfg_common_reset();

    stage = APP_WIFI_APPLY_STAGE_SET_MODE;
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_SET_MODE)) {
        err = ESP_FAIL;
        goto out_fail;
    }
#endif
    err = esp_wifi_set_mode(WIFI_MODE_STA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_cfg: set_mode STA failed: %s", esp_err_to_name(err));
        goto out_fail;
    }

    stage = APP_WIFI_APPLY_STAGE_SET_CONFIG;
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_SET_CONFIG)) {
        err = ESP_FAIL;
        goto out_fail;
    }
#endif
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_cfg: set sta config failed: %s", esp_err_to_name(err));
        goto out_fail;
    }

    if (cfg->sta_use_static) {
        err = app_configure_sta_static_ip(&stage);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply_cfg: static ip config failed: %s", esp_err_to_name(err));
            goto out_fail;
        }
    } else {
        /* A4(§10.7):DHCP client 启动硬错误属 apply 失败(阶段 dhcpc_start),
         * 不得带病返回成功 —— 救援语义要求 DHCP 真正启动。ALREADY_STARTED
         * 视为成功;netif 未 UP 时 IDF 返回 OK 并置 INIT(连接后启动)。 */
        stage = APP_WIFI_APPLY_STAGE_DHCPC_START;
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_DHCPC_START)) {
        err = ESP_FAIL;
        goto out_fail;
    }
#endif
        err = esp_netif_dhcpc_start(s_sta_netif);
        if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
            ESP_LOGE(TAG, "apply_cfg: dhcpc_start failed: %s", esp_err_to_name(err));
            goto out_fail;
        }
    }

    /* A8:置 await 后启动;门控/hold 的释放在 STA_START 处理器内完成
     * (driver 已 start 且窗口逻辑同点执行),此处不做任何提前释放。 */
    stage = APP_WIFI_APPLY_STAGE_WIFI_START;
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_failure(APP_WIFI_APPLY_STAGE_WIFI_START)) {
        err = ESP_FAIL;
        goto out_fail;
    }
#endif
    app_wifi_runtime_lock();
    s_wifi_apply_await_start = true;
    s_wifi_apply_start_consumed_gen = 0;
    s_wifi_apply_start_completed_gen = 0;
    s_wifi_apply_start_gen += 1;
    uint32_t start_gen = s_wifi_apply_start_gen;
    app_wifi_runtime_unlock();
    if (s_wifi_start_events != NULL) {
        xEventGroupClearBits(s_wifi_start_events, APP_WIFI_START_EVENT_BIT);
    }
    ESP_LOGI(TAG, "apply_await_start gen=%u", (unsigned)start_gen);

    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_cfg: start failed: %s", esp_err_to_name(err));
        app_wifi_runtime_lock();
        s_wifi_apply_await_start = false;
        app_wifi_runtime_unlock();
        goto out_fail;
    }

    /* 等待本次 START 事件:事件处理器释放门控/hold、开启动窗口并置位。
     * 超时不得强放门控(A8 裁决):按 apply 失败进入恢复链。 */
    {
        /* 以 completed_gen 为唯一完成判据(event bit 只是唤醒手段):
         * 事件已消费但窗口/调度未完成时,继续有界等待,不提前返回。 */
        int64_t deadline = app_wifi_now_ms() + APP_WIFI_START_WAIT_MS;
        bool completed = false;

        while (app_wifi_now_ms() < deadline) {
            if (s_wifi_start_events != NULL) {
                xEventGroupWaitBits(s_wifi_start_events, APP_WIFI_START_EVENT_BIT,
                                    pdTRUE, pdFALSE, pdMS_TO_TICKS(100));
            } else {
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            app_wifi_runtime_lock();
            completed = (s_wifi_apply_start_completed_gen == start_gen);
            app_wifi_runtime_unlock();
            if (completed) {
                break;
            }
        }
        if (!completed) {
            bool consumed;
            app_wifi_runtime_lock();
            consumed = (s_wifi_apply_start_consumed_gen == start_gen);
            if (!consumed) {
                s_wifi_apply_await_start = false;
            }
            app_wifi_runtime_unlock();
            if (consumed) {
                /* 已消费未完成:再给窗口/调度一个有界窗口 */
                int64_t grace = app_wifi_now_ms() + 500;
                while (app_wifi_now_ms() < grace) {
                    vTaskDelay(pdMS_TO_TICKS(20));
                    app_wifi_runtime_lock();
                    completed = (s_wifi_apply_start_completed_gen == start_gen);
                    app_wifi_runtime_unlock();
                    if (completed) { break; }
                }
            }
            if (!completed) {
                ESP_LOGE(TAG, "apply_start_timeout gen=%u, gate stays closed",
                         (unsigned)start_gen);
                err = ESP_ERR_TIMEOUT;
                stage = APP_WIFI_APPLY_STAGE_WIFI_START;
                goto out_fail;
            }
            ESP_LOGW(TAG, "apply_start_completed_late gen=%u, treat as started",
                     (unsigned)start_gen);
        }
    }

    /* A5:LKG 统一更新点 —— 只有 apply 成功出口负责 capture,
     * 调用方不再自行 capture;失败候选不可能覆盖上次成功配置。 */
    app_wifi_lkg_capture(cfg);
    if (stage_out != NULL) {
        *stage_out = APP_WIFI_APPLY_STAGE_NONE;
    }
    ESP_LOGI(TAG, "apply_cfg: started (gen=%u)", (unsigned)start_gen);
    return ESP_OK;

out_fail:
    /* A8:本 apply 已停过 driver 却未成功 start —— 必须进入离线门控,
     * 不允许失败候选留下开放连接窗口(在线 restart 同样适用)。
     * 顺序要求:同一临界区内先关门、再清 hold,不暴露 (gate=0,hold=0) 中间态。 */
    app_wifi_runtime_lock();
    bool was_offline = s_wifi.cfg_offline;
    bool hold_was = s_wifi_apply_hold;
    s_wifi.cfg_offline = true;
    s_wifi_apply_hold = false;
    app_wifi_runtime_unlock();
    if (hold_was) {
        ESP_LOGD(TAG, "apply_hold_clear");
    }
    if (!was_offline) {
        ESP_LOGW(TAG, "gate_set by apply failure stage=%s", app_wifi_apply_stage_name(stage));
    }
    if (stage_out != NULL) {
        *stage_out = stage;
    }
    return err;
}

/* 离线进入(设计稿 §3.6 固定顺序):先门控,后 quiesce,最后才允许 stop;
 * in-flight 未归零绝不 stop(R3.1)。stop 成功时统一复位连接中标志
 * (§10.2-6:esp_wifi_stop() 不保证产生 STA_DISCONNECTED)。 */
static void app_wifi_enter_offline(void)
{
    esp_err_t stop_err;

    app_wifi_runtime_lock();
    s_wifi.cfg_offline = true;
    app_wifi_runtime_unlock();
    ESP_LOGI(TAG, "offline_gate_set");

    app_wifi_stop_startup_connect_window();

    if (!app_wifi_wait_connect_inflight(APP_WIFI_QUIESCE_LIMIT_DEFAULT_MS, false)) {
        app_wifi_set_cfg_state(APP_NET_CFG_STATE_OFFLINE_QUIESCE_TIMEOUT);
        /* 门控保持;不 stop、不重试、不重启;BLE 救援可用 */
        return;
    }

    ESP_LOGI(TAG, "wifi_stop_begin");
#ifdef APP_WIFI_FAULT_INJECT
    if (app_wifi_test_take_stop_failure()) {
        stop_err = ESP_FAIL;
    } else
#endif
    {
        stop_err = esp_wifi_stop();
    }
    ESP_LOGI(TAG, "wifi_stop_end err=%s",
             stop_err == ESP_OK ? "none" : esp_err_to_name(stop_err));
    if (stop_err == ESP_OK || stop_err == ESP_ERR_WIFI_NOT_STARTED ||
        stop_err == ESP_ERR_WIFI_NOT_INIT) {
        /* §10.2-6 定稿:离线进入时统一复位连接中状态,避免残留 true
         * 抑制恢复链 apply 后的新连接调度。 */
        /* 注意:connect_inflight 不在此复位 —— 它是同步原语状态,只能由
         * begin/end 改写(§10.7-A6);quiesce 已证明其为 0,保持原值。 */
        app_wifi_runtime_lock();
        s_wifi.sta_connected = false;
        s_wifi.sta_has_ip = false;
        s_wifi.sta_ip_conflict = false;
        s_wifi.sta_retries = 0;
        app_wifi_runtime_unlock();
        s_wifi_sta_connecting = false;
        s_wifi_connecting_since_ms = 0;
        app_wifi_set_connected_ssid("");
        app_set_sta_ip_strings(NULL);
        app_wifi_runtime_lock();
        s_wifi.apply_failure.offline_stop_err = ESP_OK;
        app_wifi_runtime_unlock();
        app_wifi_set_cfg_state(APP_NET_CFG_STATE_OFFLINE);
        ESP_LOGE(TAG, "offline_pending_config: wifi stopped, BLE rescue only");
    } else {
        app_wifi_runtime_lock();
        s_wifi.apply_failure.offline_stop_err = stop_err;
        app_wifi_runtime_unlock();
        app_wifi_set_cfg_state(APP_NET_CFG_STATE_OFFLINE_STOP_FAILED);
        ESP_LOGE(TAG, "offline_stop_failed err=%s, gate remains, BLE rescue only",
                 esp_err_to_name(stop_err));
    }
}

/* 恢复链(设计稿 §3.3):LKG(仅运行期且与失败候选不同)→ DHCP 救援 → 离线;
 * 每个启动周期最多一次,禁止重启兜底,禁止重复同一失败配置。 */
static void app_wifi_recover_from_apply_failure(const app_wifi_apply_cfg_t *failed)
{
    app_wifi_apply_cfg_t rescue;
    app_wifi_apply_stage_t stage = APP_WIFI_APPLY_STAGE_NONE;
    esp_err_t err;

    /* ① LKG 恢复:仅当 LKG 有效且与失败候选不同(§1.3 硬约束) */
    if (!app_wifi_lkg_equals(failed)) {
        app_wifi_runtime_lock();
        app_wifi_apply_cfg_t lkg_copy = s_wifi.lkg;
        app_wifi_runtime_unlock();
        if (lkg_copy.valid) {
            ESP_LOGW(TAG, "recover action=lkg_restore begin");
            err = app_wifi_apply_cfg(&lkg_copy, &stage);
            if (err == ESP_OK) {
                app_wifi_runtime_lock();
                app_copy_string(s_wifi.apply_failure.recovery,
                                sizeof(s_wifi.apply_failure.recovery), "lkg_restore");
                app_wifi_runtime_unlock();
                app_wifi_set_cfg_state(lkg_copy.sta_use_static ? APP_NET_CFG_STATE_STATIC
                                                               : APP_NET_CFG_STATE_DHCP);
                ESP_LOGW(TAG, "recover action=lkg_restore result=ok");
                return;
            }
            if (stage == APP_WIFI_APPLY_STAGE_NONE) {
                ESP_LOGW(TAG, "recover action=lkg_restore abandoned (busy/drain)");
            } else {
                app_wifi_record_apply_failure(stage, err, lkg_copy.sta_use_static);
                ESP_LOGW(TAG, "recover action=lkg_restore result=fail err=%s",
                         esp_err_to_name(err));
            }
        }
    }

    /* ② DHCP 救援:沿用 SSID/口令,仅 IP 模式降级;不改 s_config/NVS */
    rescue = *failed;
    rescue.sta_use_static = false;
    ESP_LOGW(TAG, "recover action=dhcp_rescue begin");
    err = app_wifi_apply_cfg(&rescue, &stage);
    if (err == ESP_OK) {
        /* LKG 已由 apply_cfg 成功出口统一 capture(A5) */
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.apply_failure.recovery,
                        sizeof(s_wifi.apply_failure.recovery), "dhcp_rescue");
        app_wifi_runtime_unlock();
        app_wifi_set_cfg_state(APP_NET_CFG_STATE_FALLBACK);
        ESP_LOGW(TAG, "recover action=dhcp_rescue result=ok state=fallback");
        return;
    }
    if (stage == APP_WIFI_APPLY_STAGE_NONE) {
        ESP_LOGW(TAG, "recover action=dhcp_rescue abandoned (busy/drain)");
    } else {
        app_wifi_record_apply_failure(stage, err, rescue.sta_use_static);
        ESP_LOGW(TAG, "recover action=dhcp_rescue result=fail err=%s", esp_err_to_name(err));
    }

    /* ③ 离线待配置(先门控,后 stop;quiesce 超限则不 stop) */
    app_wifi_enter_offline();
}

/* 受控恢复的 quiesce 前置(R6):恢复不是绕过 quiesce 的通道。
 * 离线态下先重新等待 in-flight 归零;仍超限则拒绝本次恢复,门控保持。 */
static bool app_wifi_recover_quiesce(void)
{
    app_wifi_runtime_lock();
    bool offline = s_wifi.cfg_offline;
    app_wifi_runtime_unlock();
    if (!offline) {
        /* 非离线 restart:apply_cfg 内部的 hold+drain 提供与连接的互斥(A7) */
        return true;
    }
    /* A7 修正:只确认 quiesce,绝不清门控 —— 门控由胜出 apply 的危险段
     * 结束处(esp_wifi_start 前)统一释放;超时则拒绝本轮恢复,门控保持。 */
    return app_wifi_wait_connect_inflight(APP_WIFI_QUIESCE_LIMIT_DEFAULT_MS, true);
}

/* 启动路径(设计稿 §3.2):失败不再 abort,按恢复动作处理;
 * 平台级初始化的 ESP_ERROR_CHECK 保留在 app_start_wifi,不在本函数。 */
static void app_wifi_boot_apply_or_recover(void)
{
    app_wifi_apply_cfg_t cfg;
    app_wifi_apply_stage_t stage = APP_WIFI_APPLY_STAGE_NONE;
    esp_err_t err;

#ifdef APP_WIFI_FAULT_INJECT
    /* T5/T6:启动最早消费一次性 RTC 票据(读→校验→清→seed RAM) */
    app_wifi_boot_ticket_consume();
#endif
    /* A8:链入口取得 OP_APPLY;启动期无 tryconnect 竞争者,不等待 */
    if (!app_wifi_op_claim_apply(0)) {
        ESP_LOGW(TAG, "boot apply abandoned (op busy)");
        return;
    }
    app_wifi_build_cfg_from_config(&cfg);
    ESP_LOGI(TAG, "apply_begin mode=%s source=boot",
             cfg.sta_use_static ? "static" : "dhcp");
    err = app_wifi_apply_cfg(&cfg, &stage);
    if (err == ESP_OK) {
        /* LKG 已由 apply_cfg 成功出口统一 capture(A5) */
        app_wifi_set_cfg_state(cfg.sta_use_static ? APP_NET_CFG_STATE_STATIC
                                                  : APP_NET_CFG_STATE_DHCP);
        app_wifi_op_release_apply();
        return;
    }
    if (stage == APP_WIFI_APPLY_STAGE_NONE) {
        /* A5/A8:drain 放弃 —— 启动期无并发链,理论不可达;保持 BLE 可用 */
        ESP_LOGW(TAG, "boot apply abandoned (drain)");
        app_wifi_op_release_apply();
        return;
    }
    app_wifi_record_apply_failure(stage, err, cfg.sta_use_static);
    app_wifi_recover_from_apply_failure(&cfg);
    app_wifi_op_release_apply();
}

static esp_err_t app_wifi_perform_scan_impl(void)
{
    uint16_t number = WIFI_SCAN_LIST_SIZE;
    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err;

    scan_cfg.ssid = NULL;
    scan_cfg.bssid = NULL;
    scan_cfg.channel = 0;
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 50;
    scan_cfg.scan_time.active.max = 100;

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: start failed: %s", esp_err_to_name(err));
        return err;
    }
    err = esp_wifi_scan_get_ap_records(&number, S_WIFI_SCAN_RECORDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: get_ap_records failed: %s", esp_err_to_name(err));
        return err;
    }
    app_wifi_runtime_lock();
    s_wifi.scan_count = number;
    app_wifi_runtime_unlock();
    return ESP_OK;
}

esp_err_t app_wifi_perform_scan(void)
{
    esp_err_t err;

    app_wifi_scan_lock();
    err = app_wifi_perform_scan_impl();
    app_wifi_scan_unlock();
    return err;
}

esp_err_t app_wifi_perform_scan_command(void)
{
    esp_err_t err;

    app_wifi_auto_connect_suspend();
    if (!app_wifi_wait_connect_task_idle(15000)) {
        app_wifi_auto_connect_resume();
        ESP_LOGW(TAG, "scan command: auto-connect task still busy");
        return ESP_ERR_TIMEOUT;
    }
    err = app_wifi_perform_scan();
    app_wifi_auto_connect_resume();
    return err;
}

/* ---------------------------------------------------------------------------
 * 试连验证(1.3-37):临时换候选 SSID/口令真连一次验证口令正确性。
 * 事件归属由 tryconnect phase + 候选 SSID/BSSID 匹配决定；结束无条件恢复
 * 旧配置并交回常规自动重连路径。调用方(HTTP handler)会阻塞等待结果。
 * ------------------------------------------------------------------------- */
static app_wifi_try_result_t app_wifi_tryconnect_finish(app_wifi_try_result_t result,
                                                        bool restore_old,
                                                        wifi_config_t *old_cfg,
                                                        bool disconnect_candidate)
{
    app_wifi_try_result_t final_result;

    /* 先关闭候选事件写入权限，再 teardown；DONE 期间事件不能改结果。 */
    app_wifi_tryconnect_set_result_done(result);
    final_result = app_wifi_tryconnect_get_result();

    if (disconnect_candidate) {
        esp_wifi_disconnect();
    }
    if (restore_old && old_cfg != NULL) {
        if (esp_wifi_set_config(WIFI_IF_STA, old_cfg) != ESP_OK) {
            ESP_LOGW(TAG, "tryconnect: restore sta config failed");
        }
    }
    s_wifi_sta_connecting = false;
    app_wifi_tryconnect_release();
    app_wifi_auto_connect_resume();
    app_wifi_schedule_connect_task(pdMS_TO_TICKS(400));
    return final_result;
}

/* ---------------------------------------------------------------------------
 * 试连验证(1.3-37):临时换候选 SSID/口令真连一次验证口令正确性。
 * tryconnect 从预扫描开始持有 Wi-Fi 连接操作所有权；旧连接清理、候选启动、
 * 候选连接和 teardown 由 phase 明确分开。候选事件必须匹配候选 SSID/BSSID，
 * 信息不足的事件不能终结候选。结束无条件恢复旧配置并交回自动重连。
 * ------------------------------------------------------------------------- */
static app_wifi_try_result_t app_wifi_try_connect_common(const char *ssid, const char *password,
                                                         uint32_t timeout_ms, bool require_ip)
{
    wifi_config_t old_cfg = {0};
    wifi_config_t candidate = {0};
    uint16_t scan_count;
    bool ssid_seen = false;
    bool old_connected = false;
    bool old_connecting = false;
    bool candidate_config_set = false;
    EventGroupHandle_t events;
    esp_err_t err;
    int i;

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return APP_WIFI_TRY_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";
    }

    /* M1 R9 + A8:取得候选所有权之前拒绝(离线/apply 危险段/等待启动)。
     * 只新增一个早期返回;事件归属状态机与正常模式生命周期不变。 */
    if (app_wifi_tryconnect_entry_blocked()) {
        ESP_LOGW(TAG, "tryconnect rejected: wifi config offline/busy");
        return APP_WIFI_TRY_CONFIG_OFFLINE;
    }

    /* ownership 必须在预扫描之前建立；预扫描期间的断连不能进入普通重连。 */
    portENTER_CRITICAL(&s_tryconnect_mux);
    events = s_tryconnect_events;
    if (events == NULL) {
        events = xEventGroupCreateStatic(&s_tryconnect_events_buf);
        s_tryconnect_events = events;
    }
    portEXIT_CRITICAL(&s_tryconnect_mux);
    if (events == NULL) {
        return APP_WIFI_TRY_CONNECT_FAILED;
    }
    xEventGroupClearBits(events, APP_WIFI_TRY_DONE_BIT | APP_WIFI_TRY_OLD_DISC_BIT);
    if (!app_wifi_tryconnect_begin(ssid, require_ip)) {
        ESP_LOGW(TAG, "tryconnect busy (phase=%d)", (int)app_wifi_tryconnect_get_phase());
        return APP_WIFI_TRY_BUSY;
    }

    /* BLE 救援优先:暂停自动重连,等当前 connect task 退出后再试连。
     * ownership 已建立,期间的断连由 tryconnect helper 消费。 */
    app_wifi_auto_connect_suspend();
    if (!app_wifi_wait_connect_task_idle(15000)) {
        ESP_LOGW(TAG, "tryconnect busy: auto-connect task still running");
        return app_wifi_tryconnect_finish(APP_WIFI_TRY_BUSY, false, NULL, false);
    }

    if (esp_wifi_get_config(WIFI_IF_STA, &old_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "tryconnect: get sta config failed");
        return app_wifi_tryconnect_finish(APP_WIFI_TRY_CONNECT_FAILED, false, NULL, false);
    }

    if (app_wifi_tryconnect_cancel_get()) {
        return app_wifi_tryconnect_finish(APP_WIFI_TRY_CANCELLED, false, NULL, false);
    }

    /* 预检(方案 C→A,P1-10):扫描确认 SSID 在场。隐藏网络扫不到 —— 属预期限制,前端文案说明。
     * 扫描失败不再 fail-open:过渡态扫不到不证明 AP 不存在,更不能进入连接把驱动状态
     * 伪装成 connect_failed。失败间隔 500ms 重试一次(参数类错误不可重试);
     * 两次均失败返回 SCAN_FAILED,不启动候选连接。 */
    {
        esp_err_t scan_err = app_wifi_perform_scan();

        if (app_wifi_tryconnect_cancel_get()) {
            return app_wifi_tryconnect_finish(APP_WIFI_TRY_CANCELLED, false, NULL, false);
        }
        if (scan_err != ESP_OK && scan_err != ESP_ERR_INVALID_ARG) {
            ESP_LOGW(TAG, "tryconnect: pre-scan failed (%s), one retry in 500ms",
                     esp_err_to_name(scan_err));
            vTaskDelay(pdMS_TO_TICKS(500));
            scan_err = app_wifi_perform_scan();
        }
        if (app_wifi_tryconnect_cancel_get()) {
            return app_wifi_tryconnect_finish(APP_WIFI_TRY_CANCELLED, false, NULL, false);
        }
        if (scan_err == ESP_OK) {
            uint8_t candidate_bssid[6] = {0};
            bool candidate_bssid_valid = false;

            app_wifi_runtime_lock();
            scan_count = s_wifi.scan_count;
            app_wifi_runtime_unlock();
            for (i = 0; i < (int)scan_count; i++) {
                const char *seen = NULL;
                int8_t rssi = 0;
                uint8_t auth = 0;

                if (app_wifi_scan_record_at((uint16_t)i, &seen, &rssi, &auth) &&
                    seen != NULL && strcmp(seen, ssid) == 0) {
                    ssid_seen = true;
                    app_wifi_runtime_lock();
                    if ((uint16_t)i < s_wifi.scan_count) {
                        memcpy(candidate_bssid, S_WIFI_SCAN_RECORDS[i].bssid,
                               sizeof(candidate_bssid));
                        candidate_bssid_valid = true;
                    }
                    app_wifi_runtime_unlock();
                    break;
                }
            }
            if (!ssid_seen) {
                ESP_LOGW(TAG, "tryconnect: ssid '%s' not in scan results", ssid);
                return app_wifi_tryconnect_finish(APP_WIFI_TRY_AP_NOT_FOUND,
                                                  false, NULL, false);
            }
            if (candidate_bssid_valid) {
                app_wifi_tryconnect_state_lock();
                memcpy(s_tryconnect_candidate_bssid, candidate_bssid,
                       sizeof(s_tryconnect_candidate_bssid));
                s_tryconnect_candidate_bssid_valid = true;
                app_wifi_tryconnect_state_unlock();
            }
        } else {
            ESP_LOGW(TAG, "tryconnect: pre-scan failed twice (%s), abort before connect",
                     esp_err_to_name(scan_err));
            return app_wifi_tryconnect_finish(APP_WIFI_TRY_SCAN_FAILED,
                                              false, NULL, false);
        }
    }

    app_copy_fixed_bytes(candidate.sta.ssid, sizeof(candidate.sta.ssid), ssid);
    app_copy_fixed_bytes(candidate.sta.password, sizeof(candidate.sta.password), password);
    candidate.sta.threshold.authmode =
        (strlen(password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
    candidate.sta.pmf_cfg.capable = true;
    candidate.sta.pmf_cfg.required = false;
    candidate.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    app_wifi_runtime_lock();
    old_connected = s_wifi.sta_connected;
    app_wifi_runtime_unlock();
    old_connecting = s_wifi_sta_connecting;

    app_wifi_tryconnect_set_phase(APP_WIFI_TRY_PHASE_CLEANUP_OLD);
    s_wifi_sta_connecting = true;   /* 抑制 boot monitor 的自动调度 */
    esp_wifi_disconnect();

    if (old_connected || old_connecting) {
        app_wifi_tryconnect_set_phase(APP_WIFI_TRY_PHASE_WAIT_OLD_DISCONNECT);
        if (xEventGroupWaitBits(events, APP_WIFI_TRY_OLD_DISC_BIT, pdTRUE, pdFALSE,
                                pdMS_TO_TICKS(2000)) == 0) {
            /* 旧链路生命周期未收敛，不能启动候选。 */
            ESP_LOGW(TAG, "tryconnect: old link disconnect not observed, abort");
            return app_wifi_tryconnect_finish(APP_WIFI_TRY_CONNECT_FAILED,
                                              true, &old_cfg, false);
        }
    }

    if (app_wifi_tryconnect_cancel_get()) {
        return app_wifi_tryconnect_finish(APP_WIFI_TRY_CANCELLED, true, &old_cfg, false);
    }

    app_wifi_tryconnect_set_phase(APP_WIFI_TRY_PHASE_CANDIDATE_STARTING);
    err = esp_wifi_set_config(WIFI_IF_STA, &candidate);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tryconnect: set_config failed: %s", esp_err_to_name(err));
        return app_wifi_tryconnect_finish(APP_WIFI_TRY_CONNECT_FAILED,
                                          true, &old_cfg, false);
    }
    candidate_config_set = true;
    app_wifi_tryconnect_set_phase(APP_WIFI_TRY_PHASE_CANDIDATE_CONNECTING);
    app_wifi_log_sta_cfg("tryconnect", &candidate);
    err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tryconnect: connect failed: %s", esp_err_to_name(err));
        return app_wifi_tryconnect_finish(APP_WIFI_TRY_CONNECT_FAILED,
                                          true, &old_cfg, candidate_config_set);
    }

    {
        int64_t deadline = app_wifi_now_ms() + (int64_t)timeout_ms;
        bool cancelled = false;

        while (app_wifi_now_ms() < deadline) {
            int64_t remaining = deadline - app_wifi_now_ms();
            TickType_t wait_ticks = pdMS_TO_TICKS(remaining > 100 ? 100 : remaining);
            EventBits_t bits = xEventGroupWaitBits(events, APP_WIFI_TRY_DONE_BIT,
                                                   pdTRUE, pdFALSE, wait_ticks);
            if ((bits & APP_WIFI_TRY_DONE_BIT) != 0) {
                break;
            }
            if (app_wifi_tryconnect_cancel_get()) {
                cancelled = true;
                break;
            }
        }
        if (app_wifi_tryconnect_get_phase() != APP_WIFI_TRY_PHASE_DONE) {
            if (cancelled) {
                app_wifi_tryconnect_set_result_done(APP_WIFI_TRY_CANCELLED);
            } else {
                app_wifi_tryconnect_set_result_done(APP_WIFI_TRY_TIMEOUT);
            }
        }
    }

    /* teardown 必须先锁定 DONE，再断开候选；DONE 期间事件不能覆盖终态。 */
    return app_wifi_tryconnect_finish(APP_WIFI_TRY_CONNECT_FAILED,
                                      true, &old_cfg, candidate_config_set);
}

app_wifi_try_result_t app_wifi_try_connect(const char *ssid, const char *password,
                                           uint32_t timeout_ms)
{
    return app_wifi_try_connect_common(ssid, password, timeout_ms, false);
}

app_wifi_try_result_t app_wifi_try_connect_ip(const char *ssid, const char *password,
                                              uint32_t timeout_ms)
{
    return app_wifi_try_connect_common(ssid, password, timeout_ms, true);
}

bool app_wifi_try_cancel(void)
{
    bool cancelled = false;

    app_wifi_tryconnect_state_lock();
    if (s_tryconnect_active && s_tryconnect_phase != APP_WIFI_TRY_PHASE_IDLE) {
        s_tryconnect_cancel_requested = true;
        cancelled = true;
    }
    app_wifi_tryconnect_state_unlock();
    return cancelled;
}

static void app_wifi_restart_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        BaseType_t notified;
        app_wifi_apply_cfg_t cfg;
        app_wifi_apply_stage_t stage = APP_WIFI_APPLY_STAGE_NONE;
        esp_err_t err;

        notified = xTaskNotifyWaitIndexed(0, 0, ULONG_MAX, NULL, portMAX_DELAY);
        if (notified != pdPASS) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        /* A8:claim OP_APPLY —— 遇 tryconnect 有界等待 15s,超时放弃本轮 */
        if (!app_wifi_op_claim_apply(15000)) {
            continue;   /* APPLY_BUSY_TRYX 已记录;不算配置失败,不进恢复链 */
        }
        /* M1 R6/A8:离线态先确认 quiesce(不清门控);超限拒绝本轮 */
        if (!app_wifi_recover_quiesce()) {
            app_wifi_op_release_apply();
            continue;
        }
        app_wifi_build_cfg_from_config(&cfg);
        ESP_LOGI(TAG, "apply_begin mode=%s source=runtime_restart",
                 cfg.sta_use_static ? "static" : "dhcp");
        err = app_wifi_apply_cfg(&cfg, &stage);
        if (err == ESP_OK) {
            /* LKG 已由 apply_cfg 成功出口统一 capture(A5);门控已在
             * STA_START 处理器内释放(A8) */
            app_wifi_set_cfg_state(cfg.sta_use_static ? APP_NET_CFG_STATE_STATIC
                                                      : APP_NET_CFG_STATE_DHCP);
            app_wifi_op_release_apply();
            continue;
        }
        if (stage == APP_WIFI_APPLY_STAGE_NONE) {
            /* A5/A8:drain 放弃 —— 不算配置应用失败,不进恢复链 */
            ESP_LOGW(TAG, "restart apply abandoned (drain)");
            app_wifi_op_release_apply();
            continue;
        }
        /* 运行期 re-apply 失败走统一恢复链:LKG → DHCP 救援 → 离线(M1-03);
         * 恢复链全程持有 OP_APPLY,避免 tryconnect 在链中途插入。 */
        app_wifi_record_apply_failure(stage, err, cfg.sta_use_static);
        app_wifi_recover_from_apply_failure(&cfg);
        app_wifi_op_release_apply();
    }
}

bool app_wifi_schedule_restart(void)
{
    TaskHandle_t h;
    BaseType_t rt;
    app_wifi_restart_lock();
    h = (TaskHandle_t)s_wifi.restart_task;
    if (h != NULL) {
        app_wifi_restart_unlock();
        xTaskNotifyGive(h);
        return true;
    }
    app_wifi_restart_unlock();
    rt = xTaskCreate(app_wifi_restart_task_fn, "wifi_restart", 4096, NULL, 5, &h);
    if (rt != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi_restart task");
        return false;
    }
    app_wifi_restart_lock();
    if (s_wifi.restart_task == NULL) {
        s_wifi.restart_task = h;
    }
    app_wifi_restart_unlock();
    xTaskNotifyGive(h);
    return true;
}

void app_wifi_restart_task(void *arg)
{
    (void)arg;
    app_wifi_schedule_restart();
}

void app_start_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    const char *hostname = app_get_hostname();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* hostname 与 storage 属非致命配置,失败降级告警,不中止启动 */
    {
        esp_err_t hostname_err = esp_netif_set_hostname(s_sta_netif, hostname);
        if (hostname_err != ESP_OK) {
            ESP_LOGW(TAG, "set sta hostname failed: %s", esp_err_to_name(hostname_err));
        }
    }

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    if (s_wifi_start_events == NULL) {
        s_wifi_start_events = xEventGroupCreateStatic(&s_wifi_start_events_buf);
        if (s_wifi_start_events == NULL) {
            ESP_LOGE(TAG, "create start event group failed (apply will fail-closed)");
        }
    }
    {
        esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (storage_err != ESP_OK) {
            ESP_LOGW(TAG, "set wifi storage RAM failed: %s", esp_err_to_name(storage_err));
        }
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL));
    app_wifi_mark_profiles_dirty();
    if (s_wifi_boot_monitor_task == NULL) {
        if (xTaskCreate(app_wifi_boot_monitor_task_fn, "wifi_boot_monitor", 4096,
                        NULL, 5, &s_wifi_boot_monitor_task) != pdPASS) {
            s_wifi_boot_monitor_task = NULL;
            ESP_LOGE(TAG, "Failed to create wifi_boot_monitor task "
                          "(STA failure will stay STA for BLE rescue)");
        }
    }

    /* AP/APSTA 已移除:旧 NVS 的 net_mode 迁移为 STA,避免重启后进入无配置通道的 AP。 */
    {
        bool changed = false;
        app_config_lock();
        if (s_config.net_mode != NET_MODE_STA) {
            s_config.net_mode = NET_MODE_STA;
            changed = true;
        }
        app_config_unlock();
        if (changed) {
            app_config_lock();
            esp_err_t save_err = app_config_save(&s_config);
            app_config_unlock();
            if (save_err != ESP_OK) {
                ESP_LOGW(TAG, "force STA migration save failed: %s", esp_err_to_name(save_err));
            } else {
                ESP_LOGW(TAG, "legacy net_mode forced to STA (AP/APSTA removed)");
            }
        }
    }
    /* M1-01:启动路径移除 ESP_ERROR_CHECK abort —— 静态/配置应用失败按
     * 恢复动作处理(LKG→DHCP 救援→离线),BLE 救援始终可用,不重启循环。 */
    app_wifi_boot_apply_or_recover();
}
