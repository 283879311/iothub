#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_flash.h"
#include "esp_heap_caps.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "esp_wifi.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "auth.h"
#include "bt.h"
#include "bt_gattc.h"
#include "bt_gatts.h"
#include "cmd.h"
#include "cmd_cfg_txn.h"
#include "cmd_net_adapter.h"
#include "cmd_net_job.h"
#include "cmd_policy.h"
#include "diag.h"
#include "common.h"
#include "config.h"
#include "config_apply.h"
#include "constants.h"
#include "data_pipe.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "identity.h"
#include "mqtt.h"
#include "netcheck.h"
#include "operation.h"
#include "prov_pin.h"
#include "state.h"
#include "uart.h"
#include "wifi.h"

static const char *TAG = "cmd";

static app_operation_owner_t app_cmd_operation_owner(app_cmd_source_t source)
{
    switch (source) {
    case APP_CMD_SOURCE_BLE:  return APP_OPERATION_OWNER_BLE;
    case APP_CMD_SOURCE_HTTP: return APP_OPERATION_OWNER_HTTP;
    case APP_CMD_SOURCE_MQTT: return APP_OPERATION_OWNER_MQTT;
    default:                  return APP_OPERATION_OWNER_INTERNAL;
    }
}

extern app_config_t s_config;

static const char *const k_config_domains[] = {
    "network", "mqtt", "bluetooth", "uart", "gpio", "web",
};

/* 契约 v1.1(Phase 1):终态 stages 快照缓存(按 op id,4 槽环形,与 operation 槽数一致)。
 * 背景:job 清理后 operation.get 回落读活运行态,apply 后重建链路的数秒内四项
 * 全 false(2026-11-08 实测),与"stages=该 op 的阶段真值"语义冲突。终态后由本
 * 缓存继续提供该 op 的最后快照,响应标 stages_source="snapshot";未命中才回落
 * 活运行态并标 "live"。 */
typedef struct {
    char operation_id[APP_OPERATION_ID_LEN];
    bool valid;
    bool w;
    bool ip;
    bool nv;
    bool mc;
} app_cmd_stage_snap_t;

#define APP_CMD_STAGE_SNAP_SLOTS APP_OPERATION_MAX_SLOTS
static app_cmd_stage_snap_t s_stage_snaps[APP_CMD_STAGE_SNAP_SLOTS];
static unsigned s_stage_snap_next = 0;
static portMUX_TYPE s_stage_snap_mux = portMUX_INITIALIZER_UNLOCKED;

static void app_cmd_net_stage_snap_put(const char *op_id, bool w, bool ip, bool nv, bool mc)
{
    size_t n;
    unsigned i;

    if (op_id == NULL || op_id[0] == '\0') {
        return;
    }
    n = strlen(op_id);
    if (n >= sizeof(s_stage_snaps[0].operation_id)) {
        n = sizeof(s_stage_snaps[0].operation_id) - 1U;
    }
    portENTER_CRITICAL(&s_stage_snap_mux);
    for (i = 0; i < APP_CMD_STAGE_SNAP_SLOTS; i++) {
        if (s_stage_snaps[i].valid &&
            strncmp(s_stage_snaps[i].operation_id, op_id, n) == 0 &&
            s_stage_snaps[i].operation_id[n] == '\0') {
            s_stage_snaps[i].w = w;
            s_stage_snaps[i].ip = ip;
            s_stage_snaps[i].nv = nv;
            s_stage_snaps[i].mc = mc;
            portEXIT_CRITICAL(&s_stage_snap_mux);
            return;
        }
    }
    {
        app_cmd_stage_snap_t *slot = &s_stage_snaps[s_stage_snap_next];
        s_stage_snap_next = (s_stage_snap_next + 1U) % APP_CMD_STAGE_SNAP_SLOTS;
        memcpy(slot->operation_id, op_id, n);
        slot->operation_id[n] = '\0';
        slot->w = w;
        slot->ip = ip;
        slot->nv = nv;
        slot->mc = mc;
        slot->valid = true;
    }
    portEXIT_CRITICAL(&s_stage_snap_mux);
}

static bool app_cmd_net_stage_snap_get(const char *op_id, bool *w, bool *ip, bool *nv, bool *mc)
{
    size_t n;
    unsigned i;
    bool hit = false;

    if (op_id == NULL || op_id[0] == '\0') {
        return false;
    }
    n = strlen(op_id);
    if (n >= sizeof(s_stage_snaps[0].operation_id)) {
        n = sizeof(s_stage_snaps[0].operation_id) - 1U;
    }
    portENTER_CRITICAL(&s_stage_snap_mux);
    for (i = 0; i < APP_CMD_STAGE_SNAP_SLOTS; i++) {
        if (s_stage_snaps[i].valid &&
            strncmp(s_stage_snaps[i].operation_id, op_id, n) == 0 &&
            s_stage_snaps[i].operation_id[n] == '\0') {
            if (w != NULL) {
                *w = s_stage_snaps[i].w;
            }
            if (ip != NULL) {
                *ip = s_stage_snaps[i].ip;
            }
            if (nv != NULL) {
                *nv = s_stage_snaps[i].nv;
            }
            if (mc != NULL) {
                *mc = s_stage_snaps[i].mc;
            }
            hit = true;
            break;
        }
    }
    portEXIT_CRITICAL(&s_stage_snap_mux);
    return hit;
}
#ifdef APP_WIFI_FAULT_INJECT
/* E1 测试旋钮(仅故障注入构建):一次性覆盖下一次 operation 的 ACCEPTED TTL。
 * 护栏:clamp [500,60000];只影响 accepted_ttl,绝不触碰 GOT_IP_TTL。 */
static int s_op_ttl_override_ms = 0;
#define APP_CMD_OP_TTL_MIN_MS 500
#define APP_CMD_OP_TTL_MAX_MS 60000

/* 一次性取出并回落:只影响 ACCEPTED TTL,绝不触碰 GOT_IP_TTL。
 * DEF-NET-001(评审 P1-2):默认 45s = 15s 连接 + 25s 验证 + 余量;
 * 与非 FI 构建的硬编码值必须一致,否则 FI/非 FI 行为分叉。 */
static uint32_t app_cmd_take_accepted_ttl(void)
{
    uint32_t ttl = 45000;

    if (s_op_ttl_override_ms != 0) {
        ttl = (uint32_t)s_op_ttl_override_ms;
        s_op_ttl_override_ms = 0;
        ESP_LOGW(TAG, "op_ttl applied accepted_ttl=%ums (next op only)", (unsigned)ttl);
    }
    return ttl;
}
#endif

/* DEF-SCAN-001(2026-11-06):BLE 连接下 WiFi 主动扫描每次占射频 ~5.8s,
 * 背靠背连续调用会在 6-7 次后把 BLE 链路饿到 LL 响应超时(0x222);
 * 新旧固件同病。命令层做最小间隔节流(保底),超频返回 busy。
 * 成败都计入冷却:失败同样占用过射频。 */
#define APP_CMD_SCAN_MIN_INTERVAL_MS 3000
static int64_t s_last_scan_end_ms = 0;
static portMUX_TYPE s_scan_gate_mux = portMUX_INITIALIZER_UNLOCKED;

/* CMD-04c:敏感值擦除——volatile 逐字节写零，防优化构建消除
 * （评审要求：栈副本不可被优化掉的显式擦除）。 */
static void app_cmd_secure_wipe(char *buf, size_t len)
{
    volatile char *p = (volatile char *)(void *)buf;

    while (len--) {
        *p++ = 0;
    }
}

static cJSON *app_cmd_net_operation_status(const app_cmd_context_t *ctx, const char *operation_id, const char **err_out);

/* v1-only safety: static IP is pinned to the formal SSID. DHCP trials are not
 * allowed while the formal config is static; use BLE v1 config.set first. */
static bool app_cmd_static_switch_blocked(const char *ssid, const cJSON *params,
                                          const char **err_out)
{
    const cJSON *mode_item = cJSON_IsObject(params)
                                 ? cJSON_GetObjectItemCaseSensitive(params, "ip_mode")
                                 : NULL;
    const cJSON *leave_item = cJSON_IsObject(params)
                                  ? cJSON_GetObjectItemCaseSensitive(params, "allow_leave_static")
                                  : NULL;
    const char *mode = "keep";
    bool static_mode;
    char formal_ssid[33] = {0};

    if (leave_item != NULL) {
        *err_out = "invalid_argument";
        return true;
    }
    if (mode_item != NULL) {
        if (!cJSON_IsString(mode_item) || mode_item->valuestring == NULL) {
            *err_out = "invalid_argument";
            return true;
        }
        mode = mode_item->valuestring;
        if (strcmp(mode, "keep") != 0 && strcmp(mode, "dhcp") != 0) {
            *err_out = "invalid_argument";
            return true;
        }
    }
    app_config_lock();
    static_mode = (s_config.sta_use_static != 0);
    snprintf(formal_ssid, sizeof(formal_ssid), "%s", s_config.sta_ssid);
    app_config_unlock();
    if (!static_mode) {
        return false;
    }
    if (strcmp(ssid, formal_ssid) != 0 || strcmp(mode, "dhcp") == 0) {
        *err_out = "static_ip_pinned";
        return true;
    }
    return false;
}

void app_cmd_net_on_ble_disconnect(void)
{
    app_operation_t *op = NULL;

    /* 评审 R3:owner/op 在 job 锁内快照,消除与持锁 memset 的无同步并发读
     * (C 内存模型下属数据竞争,不只是"读到旧值")。cancel 必须在锁外调用
     * —— cancel 会经 cleanup 再取本锁,持锁调用即自锁。
     * 句柄解锁后的有效性:operation 槽位是静态数组(永不释放),槽位回收
     * 仅针对终态且需 600s 保留期(RETENTION_MS),本函数从解锁到 cancel
     * 的窗口远小于该值;cancel 内部另有 s_ops_mux 下的 generation 守卫,
     * 陈旧 finalize 不会写坏更新的 operation。 */
    (void)app_cmd_net_job_disconnect_target(&op);

    if (op != NULL) {
        app_operation_status_t status;

        if (app_operation_get_status(op, &status)) {
            if (app_op_state_can_cancel(status.state)) {
                app_operation_finish_result_t finish = app_operation_request_finish(
                    op, status.generation, APP_OP_CANCELLED,
                    APP_OPERATION_ERR_CANCELLED, APP_OPERATION_CLEANUP_REQUIRED);

                if (finish == APP_OPERATION_FINISH_DONE) {
                    app_operation_status_t final_st;
                    bool got = app_operation_get_status(op, &final_st);

                    if (got && final_st.state == APP_OP_CANCELLED) {
                        ESP_LOGI(TAG, "async network operation cancelled on BLE disconnect");
                    } else if (got) {
                        ESP_LOGW(TAG, "BLE disconnect finish done state=%s error=%s",
                                 app_op_state_name(final_st.state),
                                 app_operation_error_name(final_st.error));
                    } else {
                        ESP_LOGW(TAG, "BLE disconnect finish done but status lost");
                    }
                } else if (finish == APP_OPERATION_FINISH_PENDING) {
                    ESP_LOGW(TAG, "BLE disconnect finish pending (another cleanup owner)");
                } else if (finish == APP_OPERATION_FINISH_REJECTED) {
                    ESP_LOGW(TAG, "BLE disconnect finish rejected state=%s",
                             app_op_state_name(status.state));
                } else {
                    ESP_LOGW(TAG, "BLE disconnect finish stale gen=%u",
                             (unsigned)status.generation);
                }
                return;
            }
            if (status.state == APP_OP_COMMITTING) {
                /* 已进入提交阶段：让提交完成，结果可在重连后查询。 */
                return;
            }
        }
    }
    /* R8:trial 属主检查 + 清理入锁(NimBLE host 任务,与 worker store /
     * dispatcher 读点互斥)。 */
    if (app_cmd_net_trial_disconnect_clear()) {
        ESP_LOGI(TAG, "network trial cleared on BLE disconnect");
    }
}

/* DEF-SCAN-001 扫描节流闸(2026-11-06 返工):
 * - check 只读,不改状态;允许与否由"上一次扫描**结束**时刻"决定;
 * - stamp 在扫描返回后调用(成败都盖)。
 * 修复点:旧实现"尝试开始即盖戳",而单次扫描本身就要 ~5.8s,串行调用下
 * 两次开始间隔恒 >3s,闸门永不拒绝、等于 no-op。以结束时刻计冷却,才真正
 * 保证两次扫描之间射频空闲 ≥3s。 */
static bool app_cmd_scan_gate_check(void)
{
    int64_t now = esp_timer_get_time() / 1000;
    bool allow;

    portENTER_CRITICAL(&s_scan_gate_mux);
    allow = (s_last_scan_end_ms == 0) ||
            (now - s_last_scan_end_ms >= APP_CMD_SCAN_MIN_INTERVAL_MS);
    portEXIT_CRITICAL(&s_scan_gate_mux);
    return allow;
}

static void app_cmd_scan_gate_stamp(void)
{
    int64_t now = esp_timer_get_time() / 1000;

    portENTER_CRITICAL(&s_scan_gate_mux);
    s_last_scan_end_ms = now;
    portEXIT_CRITICAL(&s_scan_gate_mux);
}

bool app_cmd_net_op_active(void)
{
    return app_cmd_net_job_is_active();
}

static app_operation_error_t app_cmd_net_wifi_error(app_wifi_try_result_t result)
{
    switch (result) {
    case APP_WIFI_TRY_WRONG_PASSWORD: return APP_OPERATION_ERR_WRONG_PASSWORD;
    case APP_WIFI_TRY_AP_NOT_FOUND:   return APP_OPERATION_ERR_AP_NOT_FOUND;
    case APP_WIFI_TRY_SCAN_FAILED:    return APP_OPERATION_ERR_SCAN_FAILED;
    case APP_WIFI_TRY_TIMEOUT:        return APP_OPERATION_ERR_TIMEOUT;
    case APP_WIFI_TRY_CANCELLED:      return APP_OPERATION_ERR_CANCELLED;
    /* M1 R9:离线门控拒绝,独立错误,不得混入 connect_failed */
    case APP_WIFI_TRY_CONFIG_OFFLINE: return APP_OPERATION_ERR_WIFI_CONFIG_OFFLINE;
    case APP_WIFI_TRY_CLEANUP_FAILED: return APP_OPERATION_ERR_CLEANUP_FAILED;
    case APP_WIFI_TRY_OK:
    case APP_WIFI_TRY_CONNECT_FAILED:
    case APP_WIFI_TRY_BUSY:
    case APP_WIFI_TRY_INVALID_ARG:
    default:                          return APP_OPERATION_ERR_CONNECT_FAILED;
    }
}

static esp_err_t app_cmd_net_async_run(app_operation_t *op, void *ctx)
{
    app_wifi_try_result_t result;
    app_operation_status_t st0;
    app_operation_status_t st1;
    uint32_t generation;
    char ssid[APP_CMD_NET_TRIAL_SSID_LEN];
    char password[APP_CMD_NET_TRIAL_PASSWORD_LEN];
    char op_id[APP_OPERATION_ID_LEN];
    app_cmd_source_t owner;

    (void)ctx;
    if (op == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    /* CMD-03a:operation ID 局部化,后续日志/归属校验不再回读 operation 表。 */
    snprintf(op_id, sizeof(op_id), "%s", app_operation_id(op));
    if (!app_operation_get_status(op, &st0)) {
        return ESP_ERR_INVALID_ARG;
    }
    generation = st0.generation;   /* R5:跨阻塞调用校验槽位未被复用 */
    /* CMD-03 H1:通过模块快照获取 worker 运行数据,不再解引用内部 job。 */
    if (!app_cmd_net_job_worker_snapshot(op, generation, ssid, sizeof(ssid),
                                         password, sizeof(password), &owner)) {
        app_cmd_secure_wipe(password, sizeof(password));
        return ESP_ERR_INVALID_ARG;
    }
    /* B5(第三轮):活动 operation 登记——scheduled/task run 日志的 op 字段 */
    app_wifi_set_active_operation(op_id);
    ESP_LOGI(TAG, "async tryconnect ssid='%s' op=%s", ssid, op_id);
    /* DEF-MQTT-001:MQTT 源的受理回包必须先出站再拆 WiFi —— 试验的第一步
     * 就是断连扫描,一旦早于 PUBACK 就会把回包连同 TCP 一起丢掉。 */
    if (owner == APP_CMD_SOURCE_MQTT) {
        app_mqtt_resp_delivery_begin();
        app_mqtt_wait_resp_delivery();
    }
    result = app_wifi_try_connect_ip(ssid, password, 15000);

    /* R5:operation 框架已用 running 标志禁止槽位在 run_fn 返回前复用;
     * 这里再按 generation+state 复核,陈旧/已终态的 run 直接返回,绝不
     * 触碰 job(旧实现用裸 state 判断,槽位复用后会误清新 job)。 */
    if (!app_operation_get_status(op, &st1)) {
        app_cmd_secure_wipe(password, sizeof(password));
        ESP_LOGW(TAG, "async tryconnect op=%s status lost after wait, no job touch",
                 op_id);
        return ESP_ERR_INVALID_ARG;
    }
    if (st1.generation != generation || st1.state != APP_OP_CONNECTING) {
        app_cmd_secure_wipe(password, sizeof(password));
        ESP_LOGW(TAG, "async tryconnect op=%s stale after wait (gen=%u state=%d), no job touch",
                 op_id, (unsigned)generation, (int)st1.state);
        return ESP_OK;
    }
    if (result == APP_WIFI_TRY_OK) {
        app_cmd_net_trial_store_result_t stored =
            app_cmd_net_trial_store(ssid, password, owner, op_id, NULL, op, NULL);
        /* trial 保存完成后再擦除栈副本;trial 内部持有独立副本。 */
        app_cmd_secure_wipe(password, sizeof(password));

        if (stored == APP_CMD_NET_TRIAL_STALE_JOB ||
            stored == APP_CMD_NET_TRIAL_STALE_OP) {
            /* CMD-03a:cleanup 已接管终态,job 可能已被新 op 复用。
             * 不 clear_job、不写 job、不改 operation 状态。 */
            ESP_LOGW(TAG, "async tryconnect op=%s state=%d stale(%s) no job touch",
                     op_id, (int)st1.state,
                     stored == APP_CMD_NET_TRIAL_STALE_JOB ? "job_cleared" : "other_op");
            return ESP_OK;
        }
        if (stored != APP_CMD_NET_TRIAL_STORED) {
            (void)app_cmd_net_job_clear_if_owned(op, generation);
            return app_operation_set_state(op, APP_OP_CANCELLED,
                                           APP_OPERATION_ERR_CANCELLED);
        }
        /* DEF-NET-001:GOT_IP 语义升级为"IP + 联网 + MQTT 就绪"。
         * 选 B(2026-11-07 裁决):判决在试用路径的推迟窗口内出炉并按代次
         * 归档(wifi 层已等判决/到期才拆链);worker 只读自己代次的不可变
         * 快照,不读 live 状态、不再等待。无快照 = 等待上限到期 →
         * verify_timeout(裁决条件 2)。验证不过不交付 GOT_IP,不落盘。 */
        {
            app_netcheck_result_t dres;
            uint32_t link_gen = app_wifi_trial_verify_gen();
            bool have = app_netcheck_read_done(link_gen, &dres);
            bool verified = have && dres.outcome == APP_NETCHECK_OUT_OK;

            if (!app_cmd_net_job_set_netcheck_gen(op, generation, link_gen)) {
                ESP_LOGW(TAG, "async tryconnect op=%s stale before netcheck snapshot, no job touch",
                         op_id);
                return ESP_OK;
            }
            if (verified) {
                /* P6:交付快照 —— 全部取 worker 在手真值,零竞速。
                 * 2026-11-08 实锤(用户 BLE 源 apply 被拒):旧实现读活运行态,
                 * 而本流程的拆链(finish 内 esp_wifi_disconnect)会并发触发
                 * STA_DISCONNECTED(gen++/net_verified=false/sta_connected=false),
                 * 拆链后读活标志 = 自伤竞速:MQTT 源轻载 3/3 赢,BLE 连接态
                 * (NimBLE 洪泛、事件延迟 60ms)即输 → gate 误拒 → op 滞留
                 * got_ip 至 300s TTL。TRY_OK(require_ip) 本身即 wifi+ip 的
                 * 交付语义;判决真值(verified)与阶段串(dres.stage)来自
                 * read_done 不可变快照,全部确定性可得。 */
                bool st_owned = app_cmd_net_job_set_stages(
                    op, generation, true, true, verified,
                    strcmp(dres.stage, "mqtt") == 0);

                if (!st_owned) {
                    ESP_LOGW(TAG, "async tryconnect op=%s stale before stage snapshot, no job touch",
                             op_id);
                    return ESP_OK;
                }
                /* 契约 v1.1:终态快照缓存(job 清理后 operation.get 仍返回本 op
                 * 的阶段真值,不再回落活运行态)。 */
                app_cmd_net_stage_snap_put(app_operation_id(op), true, true, verified,
                                           strcmp(dres.stage, "mqtt") == 0);
            } else {
                ESP_LOGW(TAG, "async tryconnect op=%s netcheck have=%d outcome=%d gen=%u stage=%s -> rollback",
                         op_id, (int)have, (int)dres.outcome,
                         dres.gen, dres.stage);
                /* P2-4:推迟窗口内 op 可能已被 cancel/过期 —— 二次 R5 复核,
                 * 输者绝不触碰 job(与 connect 后的复核同一纪律)。 */
                if (!app_operation_get_status(op, &st1) ||
                    st1.generation != generation ||
                    st1.state != APP_OP_CONNECTING) {
                    ESP_LOGW(TAG, "async tryconnect op=%s stale after netcheck, no job touch",
                             op_id);
                    return ESP_OK;
                }
                /* 契约 v1.1:失败终态也保留"已完成阶段"真值(候选拿到过 IP
                 * 才有 netcheck 判决;wifi/ip 已达成,联网/MQTT 未达成)。 */
                app_cmd_net_stage_snap_put(app_operation_id(op), true, true, false, false);
                /* 判别点 B(state != COMMITTING → restore_persistent):清 trial/job
                 * + schedule_restart 回持久配置,再给 op 终态。 */
                (void)app_cmd_net_cleanup_run(op, NULL);
                return app_operation_set_state(
                    op, APP_OP_FAILED,
                    !have ? APP_OPERATION_ERR_VERIFY_TIMEOUT :
                    dres.outcome == APP_NETCHECK_OUT_FAIL_MQTT
                        ? APP_OPERATION_ERR_MQTT_UNAVAILABLE
                        : APP_OPERATION_ERR_INTERNET_UNVERIFIED);
            }
        }
        return app_operation_set_state(op, APP_OP_GOT_IP,
                                       APP_OPERATION_ERR_NONE);
    }

    app_cmd_secure_wipe(password, sizeof(password));
    (void)app_cmd_net_job_clear_if_owned(op, generation);
    /* 契约 v1.1:连接阶段失败/取消 = 本 op 未达成任何阶段,写入全 false 快照,
     * 避免 operation.get 回落活运行态(那是"别的链路"的状态)。 */
    app_cmd_net_stage_snap_put(app_operation_id(op), false, false, false, false);
    app_wifi_clear_active_operation(app_operation_id(op));
    if (result == APP_WIFI_TRY_CANCELLED) {
        return app_operation_set_state(op, APP_OP_CANCELLED,
                                       APP_OPERATION_ERR_CANCELLED);
    }
    return app_operation_set_state(op, APP_OP_FAILED,
                                   app_cmd_net_wifi_error(result));
}

/* 受理入口(R1-P0):接收完整上下文而非裸 source —— 快照进 job,异步执行
 * 阶段从快照派生 child。 */
static bool app_cmd_net_tryconnect_async(const cJSON *params,
                                         const app_cmd_context_t *ctx,
                                         cJSON **resp_out, const char **err_out)
{
    const cJSON *ssid_item = cJSON_IsObject(params)
                                 ? cJSON_GetObjectItemCaseSensitive(params, "ssid")
                                 : NULL;
    const cJSON *pass_item = cJSON_IsObject(params)
                                 ? cJSON_GetObjectItemCaseSensitive(params, "password")
                                 : NULL;
    const cJSON *key_item = cJSON_IsObject(params)
                                ? cJSON_GetObjectItemCaseSensitive(params, "idempotency_key")
                                : NULL;
    const char *idem_key = cJSON_IsString(key_item) ? key_item->valuestring : NULL;
    const app_cmd_source_t source = ctx->source;
    app_operation_t *op = NULL;
    /* CMD-02：受理响应只读本局部副本，不再在解锁后读共享缓冲区。 */
    char accepted_operation_id[APP_OPERATION_ID_LEN] = "";

    if (source == APP_CMD_SOURCE_MQTT && !app_cmd_mqtt_net_txn_enabled()) {
        *err_out = "guard_rejected";
        return false;
    }
    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
        ssid_item->valuestring[0] == '\0') {
        *err_out = "params_required";
        return false;
    }
    if (app_cmd_static_switch_blocked(ssid_item->valuestring, params, err_out)) {
        return false;
    }
    if (idem_key != NULL) {
        app_operation_owner_t owner = app_cmd_operation_owner(source);
        app_operation_t *existing = app_operation_find_by_key(owner, idem_key);
        if (existing != NULL) {
            cJSON *resp = app_cmd_net_operation_status(ctx, app_operation_id(existing), err_out);
            if (resp == NULL) {
                return false;
            }
            *resp_out = resp;
            return true;
        }
    }
    /* CMD-02：持 J 一次性快照 active/op/idem_key，解锁后再做幂等比较与
     * operation 查询。本处只消除数据竞争，**不宣称**检查-动作原子：
     * 快照后 job 可能被 cleanup 清零，返回旧 op 的幂等终态仍属合法结果。 */
    {
        bool job_active = false;
        app_operation_t *job_op = NULL;
        char job_idem_key[65];

        if (!app_cmd_net_job_accept_snapshot(&job_active, &job_op, job_idem_key,
                                             sizeof(job_idem_key))) {
            *err_out = "internal_error";
            return false;
        }

        if (job_active) {
            if (job_op != NULL && idem_key != NULL &&
                strcmp(job_idem_key, idem_key) == 0) {
                app_operation_status_t status;
                cJSON *resp;
                if (!app_operation_get_status(job_op, &status)) {
                    *err_out = "not_found";
                    return false;
                }
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    *err_out = "out_of_memory";
                    return false;
                }
                cJSON_AddStringToObject(resp, "operation_id", status.id);
                cJSON_AddStringToObject(resp, "state", app_op_state_name(status.state));
                *resp_out = resp;
                return true;
            }
            *err_out = "busy";
#ifdef APP_C15_BUSY_DIAG
            {
                int64_t busy_us = esp_timer_get_time();
                app_c15_busy_diag_t d;
                app_c15_busy_diag_capture(&d);
                d.same_lock_as_decision = false;
                d.snapshot_phase = "after_busy_decision";
                app_c15_busy_event_t ev = {
                    .branch = "accept", .response_code = "busy",
                    .begin_result = -1, .busy_observed = true,
                    .busy_gate = true, .begin_failure = false,
                    .mapped_busy_source = "accept_snapshot",
                    .busy_observed_us = busy_us,
                };
                app_c15_busy_diag_log(&d, &ev, NULL);
            }
#endif
            return false;
        }
    }
    if (app_cmd_net_trial_busy()) {
        *err_out = "busy";
#ifdef APP_C15_BUSY_DIAG
        {
            int64_t busy_us = esp_timer_get_time();
            app_c15_busy_diag_t d;
            app_c15_busy_diag_capture(&d);
            d.same_lock_as_decision = false;
            d.snapshot_phase = "after_busy_decision";
            app_c15_busy_event_t ev = {
                .branch = "trial", .response_code = "busy",
                .begin_result = -1, .busy_observed = true,
                .busy_gate = true, .begin_failure = false,
                .mapped_busy_source = "trial",
                .busy_observed_us = busy_us,
            };
            app_c15_busy_diag_log(&d, &ev, NULL);
        }
#endif
        return false;
    }

    app_cmd_net_job_lock();
    {
#ifdef APP_WIFI_FAULT_INJECT
        uint32_t accepted_ttl = app_cmd_take_accepted_ttl();
#else
        uint32_t accepted_ttl = 45000;
#endif
        app_cmd_net_job_begin_result_t begin = app_cmd_net_job_begin_locked(
            source, app_cmd_operation_owner(source), ctx,
            ssid_item->valuestring,
            (cJSON_IsString(pass_item) && pass_item->valuestring != NULL)
                ? pass_item->valuestring : "",
            idem_key, accepted_ttl, 300000, app_cmd_net_async_run, &op);

        if (begin != APP_CMD_NET_JOB_BEGIN_OK) {
#ifdef APP_C15_BUSY_DIAG
            int64_t busy_us = esp_timer_get_time();
            app_c15_busy_diag_t d;
            app_c15_busy_diag_capture_locked(&d);
            app_c15_begin_diag_t bd;
            app_c15_busy_diag_begin_snapshot(&bd);
            d.same_lock_as_decision = true;
            d.snapshot_phase = "in_j_before_unlock";
#endif
            app_cmd_net_job_unlock();
            *err_out = (begin == APP_CMD_NET_JOB_BEGIN_BUSY) ? "busy"
                     : (begin == APP_CMD_NET_JOB_BEGIN_NO_SLOT) ? "no_slot"
                     : (begin == APP_CMD_NET_JOB_BEGIN_NO_TXN) ? "busy"
                                                               : "internal_error";
#ifdef APP_C15_BUSY_DIAG
            {
                app_c15_busy_event_t ev = {
                    .branch = "begin", .response_code = *err_out,
                    .begin_result = (int)begin,
                    .busy_observed = (strcmp(*err_out, "busy") == 0),
                    .busy_gate = (begin == APP_CMD_NET_JOB_BEGIN_BUSY),
                    .begin_failure = (begin != APP_CMD_NET_JOB_BEGIN_BUSY),
                    .mapped_busy_source = "begin_other",
                    .busy_observed_us = busy_us,
                };
                if (begin == APP_CMD_NET_JOB_BEGIN_BUSY) {
                    ev.mapped_busy_source = bd.job_active_before
                        ? "begin_job_active" : "begin_operation_active";
                } else if (begin == APP_CMD_NET_JOB_BEGIN_NO_TXN) {
                    ev.mapped_busy_source = "begin_no_txn";
                } else if (begin == APP_CMD_NET_JOB_BEGIN_NO_SLOT) {
                    ev.mapped_busy_source = "begin_no_slot";
                }
                app_c15_busy_diag_log(&d, &ev, &bd);
            }
#endif
            return false;
        }
        snprintf(accepted_operation_id, sizeof(accepted_operation_id), "%s",
                 app_operation_id(op));
    }
    app_cmd_net_job_unlock();

    {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            *err_out = "out_of_memory";
            return false;
        }
        cJSON_AddStringToObject(resp, "operation_id", accepted_operation_id);
        cJSON_AddStringToObject(resp, "state", "accepted");
        *resp_out = resp;
    }
    return true;
}

/* CMD-04a:单次 job_lock 值快照——锁外判定与构建(锁内零 cJSON,评审约束 1/2)。
 * 字段清单与分支判定表见 docs/CMD-04a_operation_status快照清单与判定表_2026-11-14.md。 */
typedef struct {
    bool op_found;
    app_op_state_t state;
    app_operation_error_t error;
    app_operation_cleanup_state_t cleanup_state;
    char operation_id[APP_OPERATION_ID_LEN];
    app_operation_owner_t owner;
    int64_t terminal_ms;
    bool have_job_snapshot;
    bool st_wifi_connected;
    bool st_ip_acquired;
    bool st_internet_verified;
    bool st_mqtt_connected;
    bool trial_match;
    char trial_id[20];   /* 与 app_cmd_net_trial_t.id 同宽 */
    char trial_ssid[33]; /* 与 app_cmd_net_trial_t.ssid 同宽 */
} app_cmd_net_op_status_snap_t;

/* 栈预算守卫(评审约束 4):目标编译器实测 sizeof=112B(xtensa esp32 探针，
 * 2026-11-14)；断言上限 160B 防字段膨胀。调用任务 ble_cmd(8192B)/mqtt 事件
 * 任务均 >2KB 余量(low 列表未含)，net 增量约 +36B。 */
_Static_assert(sizeof(app_cmd_net_op_status_snap_t) <= 160,
               "op status snapshot exceeds stack budget");

/* 唯一 job_lock 临界区:find/get_status/job 域/trial 域一次捕获,零 cJSON。
 * 字符串全部按值复制——不含指向共享状态的指针(评审约束 1)。 */
static void app_cmd_net_op_status_capture(const char *operation_id,
                                          app_cmd_net_op_status_snap_t *snap)
{
    app_operation_t *op;
    app_operation_status_t st;
    app_cmd_net_job_snap_t job_snap;
    app_cmd_net_trial_snap_t trial_snap;

    memset(snap, 0, sizeof(*snap));

    app_cmd_net_job_lock();
    op = app_operation_find(operation_id);
    /* P1(评审):find 与 get_status 之间,终态槽位可能被其他任务的
     * operation_start 回收复用(满表覆盖/600s 到期)——get_status 读到的可能是
     * 新 operation 的状态。必须复核 st.id == 请求 id,否则本次捕获判
     * not_found:不得用新 op 的状态构造响应(否则响应 operation_id 与请求
     * 错配,且 trial/stages 与状态来自不同 op——逻辑不可能组合)。 */
    snap->op_found = (op != NULL && app_operation_get_status(op, &st) &&
                      strcmp(st.id, operation_id) == 0);
    if (snap->op_found) {
        snap->state = st.state;
        snap->error = st.error;
        snap->cleanup_state = st.cleanup_state;
        snap->owner = st.owner;
        snap->terminal_ms = st.terminal_ms;
        app_copy_string(snap->operation_id, sizeof(snap->operation_id), st.id);
    }
    app_cmd_net_job_snapshot_locked(&job_snap);
    snap->have_job_snapshot = (job_snap.active &&
                               strcmp(job_snap.operation_id, operation_id) == 0);
    if (snap->have_job_snapshot) {
        snap->st_wifi_connected = job_snap.st_wifi_connected;
        snap->st_ip_acquired = job_snap.st_ip_acquired;
        snap->st_internet_verified = job_snap.st_internet_verified;
        snap->st_mqtt_connected = job_snap.st_mqtt_connected;
    }
    app_cmd_net_trial_snapshot_locked(&trial_snap);
    snap->trial_match = (trial_snap.valid &&
                         strcmp(trial_snap.operation_id, operation_id) == 0);
    if (snap->trial_match) {
        app_copy_string(snap->trial_id, sizeof(snap->trial_id), trial_snap.id);
        app_copy_string(snap->trial_ssid, sizeof(snap->trial_ssid), trial_snap.ssid);
    }
    app_cmd_net_job_unlock();
}

static const char *app_cmd_net_cleanup_state_name(app_operation_cleanup_state_t state)
{
    switch (state) {
    case APP_OPERATION_CLEANUP_NONE:    return "none";
    case APP_OPERATION_CLEANUP_RUNNING: return "running";
    case APP_OPERATION_CLEANUP_OK:      return "ok";
    case APP_OPERATION_CLEANUP_FAIL:    return "fail";
    default:                            return "none";
    }
}

static cJSON *app_cmd_net_operation_status(const app_cmd_context_t *ctx,
                                           const char *operation_id, const char **err_out)
{
    /* CMD-04a:单次 job_lock 值快照——锁外判定与构建(锁内零 cJSON,评审约束 1/2)。
     * 字段清单与分支判定表见 docs/CMD-04a_operation_status快照清单与判定表_2026-11-14.md。 */
    app_cmd_net_op_status_snap_t snap;
    cJSON *resp;

    app_cmd_net_op_status_capture(operation_id, &snap);

    if (!snap.op_found) {
        *err_out = "not_found";
        return NULL;
    }
    if (ctx != NULL && ctx->source == APP_CMD_SOURCE_MQTT &&
        snap.owner != APP_OPERATION_OWNER_MQTT) {
        *err_out = "not_found";
        return NULL;
    }
    if (ctx != NULL && ctx->source == APP_CMD_SOURCE_HTTP &&
        snap.owner != APP_OPERATION_OWNER_HTTP) {
        *err_out = "not_found";
        return NULL;
    }
    resp = cJSON_CreateObject();
    if (resp == NULL) {
        *err_out = "out_of_memory";
        return NULL;
    }
    cJSON_AddStringToObject(resp, "operation_id", snap.operation_id);
    cJSON_AddStringToObject(resp, "state", app_op_state_name(snap.state));
    cJSON_AddStringToObject(resp, "cleanup_state",
                            app_cmd_net_cleanup_state_name(snap.cleanup_state));
    {
        /* 契约 v1.1:error 保持扁平字符串(v1 冻结);并行新增 error_detail 对象,
         * 2.0 再统一。两者值同源,客户端可任选。 */
        const char *err_name = app_operation_error_name(snap.error);
        cJSON *detail;

        cJSON_AddStringToObject(resp, "error", err_name);
        detail = cJSON_AddObjectToObject(resp, "error_detail");
        if (detail != NULL) {
            cJSON_AddStringToObject(detail, "code", err_name);
            cJSON_AddStringToObject(detail, "message", err_name);
        }
    }
    if (app_op_state_is_terminal(snap.state) && snap.terminal_ms > 0) {
        /* 契约 v1.1:终态时刻(设备时钟,与 device.status.uptime_ms 同源)。
         * 客户端复核耗时不再依赖墙钟。 */
        cJSON_AddNumberToObject(resp, "finished_uptime_ms", (double)snap.terminal_ms);
    }
    if (snap.trial_match) {
        cJSON_AddStringToObject(resp, "trial_id", snap.trial_id);
        cJSON_AddStringToObject(resp, "ssid", snap.trial_ssid);
    }
    /* DEF-NET-001(评审回复 §5):阶段结果对象,状态/错误语义不变。
     * 契约 v1.1(Phase 1)优先级:job 快照 > 终态快照缓存 > 活运行态,
     * 并以 stages_source 显式标注来源——终态后不再回落成零值。
     * live 回落的 runtime/mqtt 捕获已移出 job 锁(第三轮:消除嵌套锁段)。 */
    {
        cJSON *stages_obj = cJSON_AddObjectToObject(resp, "stages");
        bool wifi_connected;
        bool ip_acquired;
        bool net_verified;
        bool mqtt_connected;
        const char *source;

        if (snap.have_job_snapshot) {
            wifi_connected = snap.st_wifi_connected;
            ip_acquired = snap.st_ip_acquired;
            net_verified = snap.st_internet_verified;
            mqtt_connected = snap.st_mqtt_connected;
            source = "snapshot";
        } else if (app_cmd_net_stage_snap_get(operation_id, &wifi_connected, &ip_acquired,
                                              &net_verified, &mqtt_connected)) {
            source = "snapshot";
        } else {
            app_wifi_runtime_lock();
            wifi_connected = s_wifi.sta_connected;
            ip_acquired = s_wifi.sta_has_ip;
            net_verified = s_wifi.net_verified;
            app_wifi_runtime_unlock();
            app_mqtt_lock();
            mqtt_connected = s_mqtt.connected;
            app_mqtt_unlock();
            source = "live";
        }
        if (stages_obj != NULL) {
            cJSON_AddBoolToObject(stages_obj, "wifi_connected", wifi_connected);
            cJSON_AddBoolToObject(stages_obj, "ip_acquired", ip_acquired);
            cJSON_AddBoolToObject(stages_obj, "internet_verified", net_verified);
            cJSON_AddBoolToObject(stages_obj, "mqtt_connected", mqtt_connected);
        }
        cJSON_AddStringToObject(resp, "stages_source", source);
    }
    return resp;
}

static bool app_cmd_net_apply_async(const app_cmd_context_t *ctx,
                                    const cJSON *params, cJSON **resp_out,
                                    const char **err_out)
{
    const cJSON *op_item = cJSON_IsObject(params)
                               ? cJSON_GetObjectItemCaseSensitive(params, "operation_id")
                               : NULL;
    app_operation_t *op;
    app_operation_status_t status;
    cJSON *inner = NULL;
    app_cmd_result_t out = {0};
    const char *inner_err = NULL;
    bool ok = false;

    if (!cJSON_IsString(op_item) || op_item->valuestring == NULL) {
        *err_out = "params_required";
        return false;
    }
    op = app_operation_find(op_item->valuestring);
    if (op == NULL || !app_operation_get_status(op, &status)) {
        *err_out = "not_found";
        return false;
    }
    if (status.state == APP_OP_SUCCESS) {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            *err_out = "out_of_memory";
            return false;
        }
        cJSON_AddStringToObject(resp, "operation_id", status.id);
        cJSON_AddStringToObject(resp, "state", "success");
        cJSON_AddBoolToObject(resp, "saved", true);
        cJSON_AddBoolToObject(resp, "applied", true);
        *resp_out = resp;
        return true;
    }
    if (status.state != APP_OP_GOT_IP) {
        *err_out = "operation_state";
        return false;
    }
    if (app_cmd_policy_check_apply_owner(ctx, status.owner) != APP_CMD_POLICY_ALLOW) {
        *err_out = "forbidden";
        return false;
    }
    /* DEF-NET-001 兜底 v3(2026-11-07 t6 三跑定稿):提交的是"试用已验证的
     * 候选凭据",验证判决在 GOT_IP 交付时已按 operation 快照进 job(P6)。
     * 本门只读 per-op 快照——无共享态、无竞态。v1(单点查当前链路)与
     * v2(12s 轮询等重验证)在"拆链重连窗口"各输一种形态(误报
     * mqtt_unavailable / 误报 verify_timeout),根因:拆链重连后当前链路
     * 与本事务无关,查它没有意义且必然与重验证赛跑。候选网络状态在
     * 验证后到提交前的变化,由既有恢复链(LKG/救援,当日两次实证)兜底。 */
    /* CMD-02：持 J 复制 bool 后解锁判断。本处只是兜底门，非原子判定——
     * 真正的安全边界是后面的 set_state(COMMITTING) 与 expiring 闸。 */
    {
        bool internet_verified;
        app_cmd_net_job_snap_t job_snap;

        app_cmd_net_job_lock();
        app_cmd_net_job_snapshot_locked(&job_snap);
        internet_verified = job_snap.st_internet_verified;
        app_cmd_net_job_unlock();
        if (!internet_verified) {
            /* 理论不可达:GOT_IP 交付门槛保证 verified 才置位 */
            ESP_LOGW(TAG, "apply rejected: trial verification missing op=%s", status.id);
            *err_out = "verify_timeout";
            return false;
        }
    }
    {
        bool trial_ok;

        /* R8:trial 预检读点入锁;set_state(COMMITTING) 留在锁外,
         * 它与 cancel/expire 的裁决点语义不变。 */
        app_cmd_net_job_lock();
        trial_ok = app_cmd_net_trial_ready_for_operation_locked(status.id);
        app_cmd_net_job_unlock();
        if (!trial_ok) {
            *err_out = "trial_invalid";
            return false;
        }
    }
    if (app_operation_set_state(op, APP_OP_COMMITTING,
                                APP_OPERATION_ERR_NONE) != ESP_OK) {
        *err_out = "operation_state";
        return false;
    }
    /* R2-P0:COMMITTING 即申请 job 独占 —— GOT_IP TTL 过期 / BLE 断连取消的
     * cleanup 将在本锁上等待,直到 apply 以一致的终态收场再清理。
     * set_state(COMMITTING) 是与 cancel/expire 的同步点:输者(状态已被置
     * CANCELLED/EXPIRED)在此直接失败,不会进入本锁区读 parent_ctx。 */
    app_cmd_net_job_lock();

    {
        cJSON *p = cJSON_CreateObject();
        app_cmd_net_trial_snap_t trial_snap;

        app_cmd_net_trial_snapshot_locked(&trial_snap);
        if (p != NULL) {
            cJSON_AddStringToObject(p, "trial_id", trial_snap.id);
            app_cmd_net_job_set_apply_internal_locked(true);
            {
                /* C1 阶段 1(R1-P0):从受理时快照的父上下文派生 child ——
                 * source=INTERNAL、origin/trace/auth 继承 tryconnect 请求的
                 * 真实值,不由 owner 事后重建。
                 * v0 彻底删除(2026-11-05):直接按 canonical 方法名调用,
                 * 不再构造 {"method","params"} 帧。 */
                app_cmd_context_t inner_ctx;
                app_cmd_context_t parent_ctx;

                if (!app_cmd_net_job_parent_ctx_locked(&parent_ctx) ||
                    !app_cmd_ctx_internal_child(&parent_ctx, &inner_ctx)) {
                    inner_err = "internal_error";
                } else if (app_cmd_execute_method(&inner_ctx, "network.apply", p,
                                                  &inner, &out, &inner_err) == ESP_OK &&
                           inner != NULL) {
                    ok = true;
                }
            }
            app_cmd_net_job_set_apply_internal_locked(false);
            cJSON_Delete(p);
        }
    }
    cJSON_Delete(inner);
    if (ok) {
        (void)app_operation_set_state(op, APP_OP_SUCCESS, APP_OPERATION_ERR_NONE);
        app_wifi_clear_active_operation(status.id);
        app_cmd_net_job_clear_locked();
        {
            cJSON *resp = cJSON_CreateObject();
            if (resp == NULL) {
                *err_out = "out_of_memory";
                app_cmd_net_job_unlock();
                return false;
            }
            cJSON_AddStringToObject(resp, "operation_id", status.id);
            cJSON_AddStringToObject(resp, "state", "success");
            cJSON_AddBoolToObject(resp, "saved", true);
            cJSON_AddBoolToObject(resp, "applied", true);
            *resp_out = resp;
        }
        app_cmd_net_job_unlock();
        return true;
    }

    {
        app_operation_error_t error = APP_OPERATION_ERR_CONNECT_FAILED;
        if (inner_err != NULL) {
            if (strcmp(inner_err, "storage_failed") == 0) {
                error = APP_OPERATION_ERR_STORAGE_FAILED;
            } else if (strcmp(inner_err, "rollback_failed") == 0) {
                error = APP_OPERATION_ERR_ROLLBACK_FAILED;
            } else if (strcmp(inner_err, "save_failed") == 0) {
                error = APP_OPERATION_ERR_STORAGE_FAILED;
            }
        }
        (void)app_operation_set_state(op, APP_OP_FAILED, error);
        app_cmd_net_job_clear_locked();
        *err_out = inner_err != NULL ? inner_err : "apply_failed";
    }
    app_cmd_net_job_unlock();
    return false;
}

static bool app_cmd_net_cancel_async(const app_cmd_context_t *ctx,
                                     const cJSON *params, cJSON **resp_out,
                                     const char **err_out)
{
    const cJSON *op_item = cJSON_IsObject(params)
                               ? cJSON_GetObjectItemCaseSensitive(params, "operation_id")
                               : NULL;
    app_operation_t *op;
    app_operation_status_t status;

    if (!cJSON_IsString(op_item) || op_item->valuestring == NULL) {
        *err_out = "params_required";
        return false;
    }
    op = app_operation_find(op_item->valuestring);
    if (op == NULL || !app_operation_get_status(op, &status)) {
        *err_out = "not_found";
        return false;
    }
    if (!app_cmd_cancel_allowed(ctx, status.owner)) {
        *err_out = (ctx != NULL && ctx->auth_level == APP_CMD_AUTH_LOCAL_TOKEN)
                       ? "forbidden" : "unauthorized";
        return false;
    }
    if (status.state == APP_OP_CANCELLED) {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            *err_out = "out_of_memory";
            return false;
        }
        cJSON_AddStringToObject(resp, "operation_id", status.id);
        cJSON_AddStringToObject(resp, "state", "cancelled");
        *resp_out = resp;
        return true;
    }
    if (!app_op_state_can_cancel(status.state)) {
        *err_out = "operation_state";
        return false;
    }
    {
        app_operation_finish_result_t finish = app_operation_request_finish(
            op, status.generation, APP_OP_CANCELLED,
            APP_OPERATION_ERR_CANCELLED, APP_OPERATION_CLEANUP_REQUIRED);
        app_operation_status_t final_st;

        if (finish != APP_OPERATION_FINISH_DONE ||
            !app_operation_get_status(op, &final_st) ||
            final_st.state != APP_OP_CANCELLED) {
            *err_out = "operation_state";
            return false;
        }
    }
    {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            *err_out = "out_of_memory";
            return false;
        }
        cJSON_AddStringToObject(resp, "operation_id", status.id);
        cJSON_AddStringToObject(resp, "state", "cancelled");
        *resp_out = resp;
    }
    return true;
}

/* CMD-04b:network_status 四域值快照——锁内零 cJSON(评审批准的设计 §1/§3)。
 * 字段清单/判定表/资源预算见 docs/CMD-04b_network_status快照清单与判定表_2026-11-14.md；
 * sizeof=312B(目标编译器探针实测 2026-11-14)。 */
typedef struct {
    /* runtime 域（一次 app_wifi_runtime_lock，含 cfg_state 收编） */
    bool sta_connected;
    bool sta_has_ip;
    char sta_ip[16];               /* runtime.h:95 */
    char last_disconnect[32];      /* runtime.h:98 */
    char connected_ssid[33];       /* runtime.h:102 */
    int connected_profile;         /* runtime.h:103 */
    bool internet_verified;        /* = s_wifi.net_verified */
    char net_verify_stage[12];     /* runtime.h:119 */
    app_net_cfg_state_t cfg_state; /* enum 4B（收编项，skew 变化见设计 §4） */
    /* config 域（一次 app_config_lock） */
    char sta_ssid[33];             /* config.h:44 */
    bool password_set;
    uint8_t net_mode;
    bool persistent_use_static;    /* config.h:60(uint8_t)→bool */
    char persistent_static_ip[16]; /* config.h:61 */
    /* mqtt 域（一次 app_mqtt_lock） */
    bool mqtt_connected;
    char mqtt_last_error[96];      /* mqtt_runtime_t.last_error */
    /* trial 域（一次 job_lock，条件捕获——触发条件见下） */
    bool trial_ready;
    char trial_id[20];             /* app_cmd_net_trial_t.id 同宽 */
    char trial_ssid[33];
} app_cmd_network_status_snap_t;

/* 栈预算守卫：目标编译器实测 312B(kaboom 探针 2026-11-14)；上限 400B。 */
_Static_assert(sizeof(app_cmd_network_status_snap_t) <= 400,
               "network status snapshot exceeds stack budget");

static cJSON *app_cmd_build_network_status(void)
{
    app_cmd_network_status_snap_t snap;
    /* root 优先创建（设计 §3）：OOM 立即返回，不做无谓锁操作与驱动查询 */
    cJSON *root;
    cJSON *trial;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    /* nc_tcp/rssi 局部（设计 §2 不变段：API 自带锁 / 已合规模式） */
    uint32_t nc_tcp_fails_last = 0;
    int nc_tcp_last_errno = 0;
    bool rssi_valid = false;
    double rssi = -128;
    int channel = 0;
    char bssid[18] = "";

    /* ---- 捕获段（分域单锁，零 cJSON；nc_tcp/rssi 为不变段） ---- */
    app_wifi_runtime_lock();
    snap.sta_connected = s_wifi.sta_connected;
    snap.sta_has_ip = s_wifi.sta_has_ip;
    app_copy_string(snap.sta_ip, sizeof(snap.sta_ip), s_wifi.sta_ip);
    app_copy_string(snap.last_disconnect, sizeof(snap.last_disconnect),
                    s_wifi.last_disconnect);
    app_copy_string(snap.connected_ssid, sizeof(snap.connected_ssid),
                    s_wifi.connected_ssid);
    snap.connected_profile = s_wifi.connected_profile;
    snap.internet_verified = s_wifi.net_verified;
    app_copy_string(snap.net_verify_stage, sizeof(snap.net_verify_stage),
                    s_wifi.net_verify_stage);
    snap.cfg_state = s_wifi.cfg_state;   /* 收编（skew 变化见设计 §4） */
    app_wifi_runtime_unlock();

    app_netcheck_tcp_stats(&nc_tcp_fails_last, &nc_tcp_last_errno);

    {
        wifi_ap_record_t ap;

        /* P1(评审):判定用快照值，get_ap_info 与格式化全在锁外——
         * Wi-Fi 驱动查询不进 runtime 临界区（设计 §3 rssi 段口径） */
        if (snap.sta_connected && esp_wifi_sta_get_ap_info(&ap) == ESP_OK) {
            snprintf(bssid, sizeof(bssid), "%02x:%02x:%02x:%02x:%02x:%02x",
                     ap.bssid[0], ap.bssid[1], ap.bssid[2],
                     ap.bssid[3], ap.bssid[4], ap.bssid[5]);
            rssi = (double)ap.rssi;
            channel = (int)ap.primary;
            rssi_valid = true;
        }
    }

    app_config_lock();
    app_copy_string(snap.sta_ssid, sizeof(snap.sta_ssid), s_config.sta_ssid);
    snap.password_set = (s_config.sta_password[0] != '\0');
    snap.net_mode = s_config.net_mode;
    snap.persistent_use_static = (s_config.sta_use_static != 0);
    app_copy_string(snap.persistent_static_ip, sizeof(snap.persistent_static_ip),
                    s_config.sta_static_ip);
    app_config_unlock();

    app_mqtt_lock();
    snap.mqtt_connected = s_mqtt.connected;
    app_copy_string(snap.mqtt_last_error, sizeof(snap.mqtt_last_error),
                    s_mqtt.last_error);
    app_mqtt_unlock();

    /* ---- 构建段（全部锁外，键序与现状一致） ---- */
    cJSON_AddBoolToObject(root, "sta_connected", snap.sta_connected);
    cJSON_AddBoolToObject(root, "sta_has_ip", snap.sta_has_ip);
    cJSON_AddStringToObject(root, "sta_ip", snap.sta_ip);
    cJSON_AddStringToObject(root, "last_disconnect", snap.last_disconnect);
    /* 加法字段：运行态实际连上的 SSID / profile 下标。sta_ssid 是正式配置，
     * 自动回退时二者不同；App 靠这两个字段判断"当前连的是哪个网络"。 */
    cJSON_AddStringToObject(root, "connected_ssid", snap.connected_ssid);
    cJSON_AddNumberToObject(root, "connected_profile", (double)snap.connected_profile);
    /* DEF-NET-001 加法字段：联网验证结果（stage 词汇表 none|internet|mqtt|skipped） */
    cJSON_AddBoolToObject(root, "internet_verified", snap.internet_verified);
    cJSON_AddStringToObject(root, "net_verify_stage", snap.net_verify_stage);
    /* F-1:最近一次 netcheck 的 TCP 探测失败计数与 errno(双向证据)。 */
    cJSON_AddNumberToObject(root, "nc_tcp_fails_last", (double)nc_tcp_fails_last);
    cJSON_AddNumberToObject(root, "nc_tcp_last_errno", (double)nc_tcp_last_errno);
    /* 当前 AP 的 RSSI/信道/BSSID(加法字段)。断连时 rssi=-128, rssi_valid=false。 */
    cJSON_AddNumberToObject(root, "rssi", rssi);
    cJSON_AddNumberToObject(root, "channel", (double)channel);
    cJSON_AddStringToObject(root, "bssid", bssid);
    cJSON_AddBoolToObject(root, "rssi_valid", rssi_valid);
    cJSON_AddStringToObject(root, "sta_ssid", snap.sta_ssid);
    cJSON_AddBoolToObject(root, "password_set", snap.password_set);
    cJSON_AddStringToObject(root, "mode", app_network_mode_to_string(snap.net_mode));
    /* 持久静态规划 + 运行期配置状态:救援降级时客户端可提示。 */
    cJSON_AddBoolToObject(root, "persistent_use_static", snap.persistent_use_static);
    cJSON_AddStringToObject(root, "persistent_static_ip", snap.persistent_static_ip);
    cJSON_AddStringToObject(root, "cfg_state",
                            app_wifi_cfg_state_name_public(snap.cfg_state));
    cJSON_AddBoolToObject(root, "mqtt_connected", snap.mqtt_connected);
    cJSON_AddStringToObject(root, "mqtt_last_error", snap.mqtt_last_error);

    /* trial 对象最后创建：保持 wire 键序；trial_ready（含过期清理副作用）
     * 只在对象创建成功后执行——触发条件与现状逐字一致。 */
    trial = cJSON_AddObjectToObject(root, "trial");
    if (trial != NULL) {
        /* R8:trial 读点入锁(与 worker store / 断连 clear 跨任务)。 */
        app_cmd_net_job_lock();
        snap.trial_ready = app_cmd_net_trial_ready_locked(NULL);
        if (snap.trial_ready) {
            app_cmd_net_trial_snap_t trial_snap;

            app_cmd_net_trial_snapshot_locked(&trial_snap);
            app_copy_string(snap.trial_id, sizeof(snap.trial_id), trial_snap.id);
            app_copy_string(snap.trial_ssid, sizeof(snap.trial_ssid), trial_snap.ssid);
        }
        app_cmd_net_job_unlock();
        if (snap.trial_ready) {
            cJSON_AddStringToObject(trial, "trial_id", snap.trial_id);
            cJSON_AddStringToObject(trial, "state", "got_ip");
            cJSON_AddStringToObject(trial, "ssid", snap.trial_ssid);
        } else {
            /* 原 idle 分支：只有 state，不输出 trial_id/ssid（P0 修正：
             * 未 ready 时快照字符串未赋值，不得读取） */
            cJSON_AddStringToObject(trial, "state", "idle");
        }
    }
    return root;
}

static const char *app_cmd_reset_reason_name(esp_reset_reason_t reason)
{
    switch (reason) {
    case ESP_RST_POWERON:  return "poweron";
    case ESP_RST_EXT:      return "external";
    case ESP_RST_SW:       return "software";
    case ESP_RST_PANIC:    return "panic";
    case ESP_RST_INT_WDT:  return "int_wdt";
    case ESP_RST_TASK_WDT: return "task_wdt";
    case ESP_RST_WDT:      return "wdt";
    case ESP_RST_DEEPSLEEP:return "deepsleep";
    case ESP_RST_BROWNOUT: return "brownout";
    case ESP_RST_SDIO:     return "sdio";
    default:               return "unknown";
    }
}

static const char *app_cmd_ota_state_name(esp_ota_img_states_t state)
{
    switch (state) {
    case ESP_OTA_IMG_NEW:             return "new";
    case ESP_OTA_IMG_PENDING_VERIFY:  return "pending_verify";
    case ESP_OTA_IMG_VALID:           return "valid";
    case ESP_OTA_IMG_INVALID:         return "invalid";
    case ESP_OTA_IMG_ABORTED:         return "aborted";
    default:                          return "undefined";
    }
}

static cJSON *app_cmd_build_gpio_status(void)
{
    app_io_state_t io;
    cJSON *root;
    cJSON *pins;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    uint8_t relay_policy;
    uint8_t led_policy;

    app_io_get_state(&io);
    app_config_lock();
    relay_active_high = s_config.relay_active_high;
    led_active_high = s_config.led_active_high;
    input_active_low = s_config.input_active_low;
    relay_policy = s_config.relay_power_policy;
    led_policy = s_config.led_power_policy;
    app_config_unlock();

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "relay_on", io.relay_on);
    cJSON_AddBoolToObject(root, "led_on", io.led_on);
    cJSON_AddBoolToObject(root, "input_active", io.input_active);
    cJSON_AddNumberToObject(root, "io_seq", (double)io.seq);
    cJSON_AddNumberToObject(root, "io_last_change_ms", (double)io.last_change_ms);
    cJSON_AddStringToObject(root, "io_source", app_io_source_name(io.last_source));
    cJSON_AddBoolToObject(root, "relay_active_high", relay_active_high != 0);
    cJSON_AddBoolToObject(root, "led_active_high", led_active_high != 0);
    cJSON_AddBoolToObject(root, "input_active_low", input_active_low != 0);
    cJSON_AddNumberToObject(root, "relay_power_policy", (double)relay_policy);
    cJSON_AddNumberToObject(root, "led_power_policy", (double)led_policy);
    pins = cJSON_AddObjectToObject(root, "pins");
    if (pins != NULL) {
        cJSON_AddNumberToObject(pins, "relay", (double)RELAY_DEFAULT_GPIO);
        cJSON_AddNumberToObject(pins, "led", (double)LED_DEFAULT_GPIO);
        cJSON_AddNumberToObject(pins, "input", (double)INPUT_DEFAULT_GPIO);
    }
    return root;
}

static cJSON *app_cmd_build_uart_runtime(void)
{
    app_uart_status_t st;
    cJSON *root;
    uint32_t baudrate;
    uint8_t data_bits;
    uint8_t stop_bits;
    uint8_t parity;
    char frame[16] = "";
    bool runtime_ok;

    runtime_ok = app_uart_get_status(&st);
    app_config_lock();
    baudrate = s_config.uart_baudrate;
    data_bits = s_config.uart_data_bits;
    stop_bits = s_config.uart_stop_bits;
    parity = s_config.uart_parity_mode;
    app_config_unlock();
    app_uart_build_frame(frame, sizeof(frame), data_bits, (uart_parity_t)parity, stop_bits);

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddBoolToObject(root, "runtime_available", runtime_ok);
    cJSON_AddNumberToObject(root, "baudrate", (double)baudrate);
    cJSON_AddNumberToObject(root, "data_bits", (double)data_bits);
    cJSON_AddNumberToObject(root, "stop_bits", (double)stop_bits);
    cJSON_AddNumberToObject(root, "parity", (double)parity);
    cJSON_AddStringToObject(root, "frame", frame);
    cJSON_AddNumberToObject(root, "rx_pin", (double)UART_RX_GPIO);
    cJSON_AddNumberToObject(root, "tx_pin", (double)UART_TX_GPIO);
    if (runtime_ok) {
        cJSON_AddBoolToObject(root, "driver_installed", st.driver_installed);
        cJSON_AddNumberToObject(root, "rx_sequence", (double)st.rx_sequence);
        cJSON_AddNumberToObject(root, "tx_sequence", (double)st.tx_sequence);
        cJSON_AddNumberToObject(root, "last_bytes_sent", (double)st.last_bytes_sent);
        cJSON_AddNumberToObject(root, "last_send_at_ms", (double)st.last_send_at_ms);
        cJSON_AddStringToObject(root, "last_apply_error", st.last_apply_error);
        cJSON_AddStringToObject(root, "last_tx_encoding", st.last_tx_encoding);
        cJSON_AddStringToObject(root, "last_tx_preview", st.last_tx_preview);
    }
    return root;
}

/* 运行镜像长度（device.status.partition.used/free 用）。
 * esp_image_get_metadata 只解析分段头、不做 SHA 校验，比 esp_image_verify 便宜；
 * 结果进程内缓存，避免每次 status 都读 flash。返回 0 = 取不到（字段省略）。 */
static size_t app_cmd_running_image_len(void)
{
    static size_t cached = 0;
    const esp_partition_t *p;
    esp_partition_pos_t pos;
    esp_image_metadata_t meta;

    if (cached != 0) {
        return cached;
    }
    p = esp_ota_get_running_partition();
    if (p == NULL) {
        return 0;
    }
    pos.offset = p->address;
    pos.size = p->size;
    if (esp_image_get_metadata(&pos, &meta) != ESP_OK || meta.image_len == 0) {
        return 0;
    }
    cached = (size_t)meta.image_len;
    return cached;
}

static cJSON *app_cmd_build_device_status(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root;
    cJSON *heap;
    cJSON *ble;
    cJSON *mqtt;
    cJSON *partition;
    cJSON *network;
    uint8_t bt_mode = 0;
    char bt_error[64] = "";
    bool mqtt_connected;
    char mqtt_error[sizeof(s_mqtt.last_error)];
    const esp_partition_t *running;
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", desc != NULL ? desc->project_name : "iothub");
    cJSON_AddStringToObject(root, "firmware", desc != NULL ? desc->version : "");
    /* 2026-11-06:脏树构建时 firmware 串不随代码变化(所有镜像都报同一 "-dirty"),
     * 现场无法区分版本。build_sha 取 app descriptor 内嵌的 ELF SHA-256 前
     * CONFIG_APP_RETRIEVE_LEN_ELF_SHA 位(随配置,当前 9),
     * 与二进制一一对应;旧客户端忽略该加法字段。 */
    {
        /* 长度由 CONFIG_APP_RETRIEVE_LEN_ELF_SHA 决定(当前 9 位十六进制),
         * 不做定长假设;非空即上报。 */
        const char *elf_sha = esp_app_get_elf_sha256_str();

        if (elf_sha != NULL && elf_sha[0] != '\0') {
            cJSON_AddStringToObject(root, "build_sha", elf_sha);
        }
    }
    cJSON_AddStringToObject(root, "device_id", app_get_hostname());
    cJSON_AddNumberToObject(root, "uptime_ms", (double)(esp_timer_get_time() / 1000));
    cJSON_AddStringToObject(root, "reset_reason",
                            app_cmd_reset_reason_name(esp_reset_reason()));

    heap = cJSON_AddObjectToObject(root, "heap");
    if (heap != NULL) {
        cJSON_AddNumberToObject(heap, "free", (double)esp_get_free_heap_size());
        cJSON_AddNumberToObject(heap, "min_free",
                                (double)esp_get_minimum_free_heap_size());
        cJSON_AddNumberToObject(heap, "largest",
                                (double)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        cJSON_AddNumberToObject(heap, "internal_largest",
                                (double)heap_caps_get_largest_free_block(
                                    MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT));
    }

    /* 任务栈水位（2026-11-14）：min_free = 表内任务历史最低剩余的最小值；
     * low = <2KB 的任务列表，空则省略。加法字段，旧客户端忽略。 */
    {
        cJSON *stack = cJSON_AddObjectToObject(root, "stack");
        if (stack != NULL) {
            size_t min_bytes = 0;
            const char *min_task = "?";
            char low[160] = "";

            if (app_diag_stack_snapshot(&min_bytes, &min_task, low, sizeof(low))) {
                cJSON_AddNumberToObject(stack, "min_free", (double)min_bytes);
                cJSON_AddStringToObject(stack, "min_task", min_task);
                if (low[0] != '\0') {
                    cJSON_AddStringToObject(stack, "low", low);
                }
            }
        }
    }

    app_bt_get_status(&bt_mode, bt_error, sizeof(bt_error));
    ble = cJSON_AddObjectToObject(root, "ble");
    if (ble != NULL) {
        cJSON_AddBoolToObject(ble, "advertising", app_bt_advertising());
        cJSON_AddBoolToObject(ble, "connected", app_ble_gatts_connected());
        cJSON_AddNumberToObject(ble, "mtu", (double)app_ble_gatts_mtu());
        cJSON_AddNumberToObject(ble, "adv_retry_count",
                                (double)app_bt_adv_retry_count());
        cJSON_AddNumberToObject(ble, "runtime_mode", (double)bt_mode);
        cJSON_AddStringToObject(ble, "last_error", bt_error);
        /* DEF-BLE-001 诊断:cf=0 表示协议栈仍持有链路(与 connected 对照可判
         * "应用层连着但控制器已断" 或反之);rx_age_ms=-1 表示本次连接无写入。 */
        cJSON_AddNumberToObject(ble, "conn_find", (double)app_ble_gatts_conn_find());
        cJSON_AddNumberToObject(ble, "rx_age_ms", (double)app_ble_gatts_rx_age_ms());
    }

    app_mqtt_lock();
    mqtt_connected = s_mqtt.connected;
    memcpy(mqtt_error, s_mqtt.last_error, sizeof(mqtt_error));
    app_mqtt_unlock();
    mqtt = cJSON_AddObjectToObject(root, "mqtt");
    if (mqtt != NULL) {
        cJSON_AddBoolToObject(mqtt, "connected", mqtt_connected);
        cJSON_AddBoolToObject(mqtt, "connecting", app_mqtt_connecting());
        cJSON_AddStringToObject(mqtt, "last_error", mqtt_error);
        cJSON_AddNumberToObject(mqtt, "outbox", (double)app_mqtt_outbox_size());
        cJSON_AddNumberToObject(mqtt, "telemetry_shed",
                                (double)app_mqtt_tele_shed_count());
    }

    running = esp_ota_get_running_partition();
    partition = cJSON_AddObjectToObject(root, "partition");
    if (partition != NULL && running != NULL) {
        uint32_t flash_size = 0;
        size_t image_len = app_cmd_running_image_len();

        cJSON_AddStringToObject(partition, "label", running->label);
        cJSON_AddNumberToObject(partition, "address", (double)running->address);
        cJSON_AddNumberToObject(partition, "size", (double)running->size);
        if (image_len > 0) {
            cJSON_AddNumberToObject(partition, "used", (double)image_len);
            if (running->size > image_len) {
                cJSON_AddNumberToObject(partition, "free", (double)(running->size - image_len));
            }
        }
        /* 整片 flash 容量（4MB 等）；分区 size 之外还有 bootloader/分区表/otadata。
         * 加法字段，旧客户端忽略。 */
        if (esp_flash_get_size(NULL, &flash_size) == ESP_OK && flash_size > 0) {
            cJSON_AddNumberToObject(partition, "flash_total", (double)flash_size);
        }
        if (esp_ota_get_state_partition(running, &ota_state) == ESP_OK) {
            cJSON_AddStringToObject(partition, "ota_state",
                                    app_cmd_ota_state_name(ota_state));
        }
    }

    network = app_cmd_build_network_status();
    if (network != NULL) {
        cJSON_AddItemToObject(root, "network", network);
    }
    return root;
}

/* ---------------------------------------------------------------------------
 * 护栏:云端/BLE 命令面可改网络参数的开关 —— 默认关闭(改错会自断通道,只能现场恢复)。
 * 开启需改此宏重编(不做远端/网页开关,防止"云端自己开自己")。1.3-31/1.3-32 共用。
 * ------------------------------------------------------------------------- */
#define APP_CLOUD_ALLOW_NET_CMDS 0

static bool app_cmd_patch_allowed(app_cmd_source_t source, const cJSON *params)
{
#if APP_CLOUD_ALLOW_NET_CMDS
    (void)source;
    (void)params;
    return true;
#else
    const cJSON *item = NULL;

    /* HTTP(配置页)和 BLE(本地配网)允许改网络/MQTT/BT/UART/管理员字段;
     * MQTT 下发默认禁止这些本地/身份相关域,避免远端改错 host/token、
     * 改 Wi-Fi、改 UART/BT 或替换管理员用户名后把自己锁在门外。
     * 注意:GPIO 控制和 data_route 仍按现有产品策略允许 MQTT。 */
    if (source != APP_CMD_SOURCE_MQTT) {
        return true;
    }
    if (!cJSON_IsObject(params)) {
        return true;   /* 非法 params 由统一层报错 */
    }
    cJSON_ArrayForEach(item, params) {
        const cfg_field_t *f = config_apply_field(item->string != NULL ? item->string : "");

        if (f != NULL &&
            (f->domain == CFG_DOMAIN_NET || f->domain == CFG_DOMAIN_MQTT ||
             f->domain == CFG_DOMAIN_BT || f->domain == CFG_DOMAIN_UART ||
             f->domain == CFG_DOMAIN_AUTH)) {
            ESP_LOGW(TAG, "remote patch rejected (guard): field '%s' domain=%d",
                     item->string, (int)f->domain);
            return false;
        }
    }
    return true;
#endif
}

static bool app_cmd_verify_auth_password(const char *password)
{
    uint8_t salt[16];
    uint8_t stored[32];
    uint8_t calc[32];
    uint8_t diff = 0;
    size_t i;

    if (password == NULL) {
        return false;
    }
    app_config_lock();
    memcpy(salt, s_config.auth_pass_salt, sizeof(salt));
    memcpy(stored, s_config.auth_pass_hash, sizeof(stored));
    app_config_unlock();
    app_auth_hash_password(salt, sizeof(salt), password, calc);
    for (i = 0; i < sizeof(calc); i++) {
        diff |= (uint8_t)(calc[i] ^ stored[i]);
    }
    return diff == 0;
}

static bool app_cmd_parse_bool_param(const cJSON *params, bool fallback)
{
    const cJSON *p = NULL;
    if (params == NULL) {
        return fallback;
    }
    if (cJSON_IsObject(params)) {
        p = cJSON_GetObjectItemCaseSensitive(params, "value");
    }
    if (p == NULL) {
        p = params;
    }
    if (cJSON_IsBool(p)) {
        return cJSON_IsTrue(p);
    }
    if (cJSON_IsNumber(p)) {
        return p->valueint != 0;
    }
    return fallback;
}

static bool app_cmd_domain_known(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(k_config_domains) / sizeof(k_config_domains[0]); i++) {
        if (strcmp(name, k_config_domains[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* config.get domains 过滤:domains 省略表示全部;未知域名由调用方先拒绝 */
static bool app_cmd_domain_wanted(const cJSON *domains, const char *name)
{
    const cJSON *item = NULL;

    if (domains == NULL) {
        return true;
    }
    cJSON_ArrayForEach(item, domains) {
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            strcmp(item->valuestring, name) == 0) {
            return true;
        }
    }
    return false;
}

/* getConfig 的脱敏汇总:与 HTTP GET 同规则 —— 口令/token 永不回明文,只回 *_set */
static cJSON *app_cmd_build_get_config(const cJSON *domains)
{
    app_config_t cfg;
    cJSON *root;
    cJSON *sec;
    const char *parity;

    app_config_lock();
    cfg = s_config;
    app_config_unlock();

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    sec = cJSON_AddObjectToObject(root, "network");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "net_mode", app_network_mode_to_string(cfg.net_mode));
        cJSON_AddStringToObject(sec, "sta_ssid", cfg.sta_ssid);
        cJSON_AddBoolToObject(sec, "sta_password_set", cfg.sta_password[0] != '\0');
        cJSON_AddBoolToObject(sec, "sta_use_static", cfg.sta_use_static != 0);
        cJSON_AddStringToObject(sec, "sta_static_ip", cfg.sta_static_ip);
        cJSON_AddStringToObject(sec, "sta_static_gateway", cfg.sta_static_gateway);
        cJSON_AddStringToObject(sec, "sta_static_netmask", cfg.sta_static_netmask);
        cJSON_AddStringToObject(sec, "sta_static_dns1", cfg.sta_static_dns1);
        cJSON_AddStringToObject(sec, "sta_static_dns2", cfg.sta_static_dns2);
        {
            bool sta_connected;
            bool sta_has_ip;
            char sta_ip[16];
            char last_disconnect[32];

            app_wifi_runtime_lock();
            sta_connected = s_wifi.sta_connected;
            sta_has_ip = s_wifi.sta_has_ip;
            memcpy(sta_ip, s_wifi.sta_ip, sizeof(sta_ip));
            memcpy(last_disconnect, s_wifi.last_disconnect, sizeof(last_disconnect));
            app_wifi_runtime_unlock();
            cJSON_AddBoolToObject(sec, "sta_connected", sta_connected);
            cJSON_AddBoolToObject(sec, "sta_has_ip", sta_has_ip);
            cJSON_AddStringToObject(sec, "sta_ip", sta_ip);
            cJSON_AddStringToObject(sec, "last_disconnect", last_disconnect);
        }
    }

    sec = cJSON_AddObjectToObject(root, "mqtt");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "mqtt_backend", cfg.mqtt_backend);
        cJSON_AddStringToObject(sec, "mqtt_host", cfg.mqtt_host);
        cJSON_AddNumberToObject(sec, "mqtt_port", (double)cfg.mqtt_port);
        cJSON_AddBoolToObject(sec, "mqtt_use_tls", cfg.mqtt_use_tls != 0);
        cJSON_AddBoolToObject(sec, "mqtt_token_set", cfg.mqtt_token[0] != '\0');
        {
            bool connected;
            uint32_t publish_count;
            char last_error[sizeof(s_mqtt.last_error)];

            app_mqtt_lock();
            connected = s_mqtt.connected;
            publish_count = s_mqtt.publish_count;
            memcpy(last_error, s_mqtt.last_error, sizeof(last_error));
            app_mqtt_unlock();
            cJSON_AddBoolToObject(sec, "connected", connected);
            cJSON_AddNumberToObject(sec, "publish_count", (double)publish_count);
            cJSON_AddNumberToObject(sec, "outbox", (double)app_mqtt_outbox_size());
            cJSON_AddStringToObject(sec, "last_error", last_error);
        }
    }

    sec = cJSON_AddObjectToObject(root, "bluetooth");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "bt_role", app_bt_mode_to_string(cfg.bt_role));
        cJSON_AddStringToObject(sec, "bt_device_name", cfg.bt_device_name);
        {
            uint8_t runtime_mode = cfg.bt_role;
            char last_error[96] = {0};
            bool connected;

            app_bt_get_status(&runtime_mode, last_error, sizeof(last_error));
            connected = app_ble_gatts_connected() || app_ble_gattc_connected();
            cJSON_AddStringToObject(sec, "runtime_mode", app_bt_mode_to_string(runtime_mode));
            cJSON_AddBoolToObject(sec, "adv_active", app_bt_advertising());
            cJSON_AddBoolToObject(sec, "connected", connected);
            cJSON_AddNumberToObject(sec, "adv_retry_count", (double)app_bt_adv_retry_count());
            cJSON_AddStringToObject(sec, "last_error", last_error);
        }
    }

    sec = cJSON_AddObjectToObject(root, "uart");
    if (sec != NULL) {
        parity = (cfg.uart_parity_mode == UART_PARITY_EVEN) ? "even"
               : (cfg.uart_parity_mode == UART_PARITY_ODD) ? "odd"
               : "none";
        cJSON_AddNumberToObject(sec, "uart_baudrate", (double)cfg.uart_baudrate);
        cJSON_AddNumberToObject(sec, "uart_data_bits", (double)cfg.uart_data_bits);
        cJSON_AddNumberToObject(sec, "uart_stop_bits", (double)cfg.uart_stop_bits);
        cJSON_AddStringToObject(sec, "uart_parity_mode", parity);
        cJSON_AddNumberToObject(sec, "uart_rx_gpio", (double)app_uart_rx_gpio());
        cJSON_AddNumberToObject(sec, "uart_tx_gpio", (double)app_uart_tx_gpio());
    }

    sec = cJSON_AddObjectToObject(root, "gpio");
    if (sec != NULL) {
        app_io_state_t io;

        app_io_get_state(&io);
        cJSON_AddBoolToObject(sec, "relay_on", io.relay_on);
        cJSON_AddBoolToObject(sec, "led_on", io.led_on);
        cJSON_AddNumberToObject(sec, "relay_power_policy", cfg.relay_power_policy);
        cJSON_AddNumberToObject(sec, "led_power_policy", cfg.led_power_policy);
        cJSON_AddBoolToObject(sec, "relay_active_high", cfg.relay_active_high != 0);
        cJSON_AddBoolToObject(sec, "led_active_high", cfg.led_active_high != 0);
        cJSON_AddBoolToObject(sec, "input_active_low", cfg.input_active_low != 0);
        cJSON_AddBoolToObject(sec, "input_active", io.input_active);
        cJSON_AddNumberToObject(sec, "io_seq", io.seq);
        cJSON_AddNumberToObject(sec, "relay_gpio", (double)app_relay_gpio());
        cJSON_AddNumberToObject(sec, "led_gpio", (double)app_led_gpio());
        cJSON_AddNumberToObject(sec, "input_gpio", (double)app_input_gpio());
    }

    sec = cJSON_AddObjectToObject(root, "web");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "username", cfg.auth_username);
    }

    if (domains != NULL) {
        size_t i;

        for (i = 0; i < sizeof(k_config_domains) / sizeof(k_config_domains[0]); i++) {
            if (!app_cmd_domain_wanted(domains, k_config_domains[i])) {
                cJSON_DeleteItemFromObjectCaseSensitive(root, k_config_domains[i]);
            }
        }
    }

    return root;
}

/* v1.0 探测入口：无副作用，不暴露凭据；security 事实先如实报告 false。 */
static cJSON *app_cmd_build_device_info(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    cJSON *versions;
    cJSON *security;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", desc != NULL ? desc->project_name : "iothub");
    cJSON_AddStringToObject(root, "firmware", desc != NULL ? desc->version : "");
    /* 2026-11-06:脏树构建时 firmware 串不随代码变化,现场无法区分版本。
     * build_sha 取 app descriptor 内嵌 ELF SHA-256 前 CONFIG_APP_RETRIEVE_LEN_ELF_SHA 位(随配置,当前 9),
     * 与二进制一一对应;纯加法字段,旧客户端忽略。 */
    {
        const char *elf_sha = esp_app_get_elf_sha256_str();

        if (elf_sha != NULL && elf_sha[0] != '\0') {
            cJSON_AddStringToObject(root, "build_sha", elf_sha);
        }
    }
    cJSON_AddStringToObject(root, "device_id", app_get_hostname());
    cJSON_AddStringToObject(root, "build_variant", app_cmd_policy_build_variant());
    versions = cJSON_AddArrayToObject(root, "protocol_versions");
    if (versions != NULL) {
        cJSON_AddItemToArray(versions, cJSON_CreateString("1.0"));
    }
    security = cJSON_AddObjectToObject(root, "security");
    if (security != NULL) {
        cJSON_AddBoolToObject(security, "link_encrypted", false);
        cJSON_AddBoolToObject(security, "bonded", false);
        cJSON_AddBoolToObject(security, "l2_ready", false);
        cJSON_AddBoolToObject(security, "sensitive_writes_allowed", false);
    }
    return root;
}

/* 普通结果(含 success 外壳):changed 移交所有权 */
static cJSON *app_cmd_build_success(cJSON *changed, cfg_effect_t effect)
{
    cJSON *resp = cJSON_CreateObject();
    bool relay_on;
    bool led_on;

    if (resp == NULL) {
        if (changed != NULL) {
            cJSON_Delete(changed);
        }
        return NULL;
    }
    cJSON_AddBoolToObject(resp, "success", true);
    if (changed != NULL) {
        cJSON_AddItemToObject(resp, "changed", changed);
    } else {
        cJSON_AddItemToObject(resp, "changed", cJSON_CreateArray());
    }
    cJSON_AddStringToObject(resp, "effect",
                            effect == CFG_EFFECT_REBOOT ? "reboot"
                          : effect == CFG_EFFECT_SUBSYS ? "subsys"
                          : "none");
    {
        app_io_state_t io;

        app_io_get_state(&io);
        relay_on = io.relay_on;
        led_on = io.led_on;
        cJSON_AddBoolToObject(resp, "relay_on", relay_on);
        cJSON_AddBoolToObject(resp, "led_on", led_on);
        cJSON_AddBoolToObject(resp, "input_active", io.input_active);
        cJSON_AddNumberToObject(resp, "io_seq", io.seq);
        cJSON_AddStringToObject(resp, "io_source", app_io_source_name(io.last_source));
    }
    return resp;
}

/* config.set 的统一事务字段;applied 只表示运行态已接受配置,不表示服务健康 */
static void app_cmd_add_txn_fields(cJSON *resp, cfg_effect_t effect)
{
    if (resp == NULL) {
        return;
    }
    cJSON_AddBoolToObject(resp, "saved", true);
    cJSON_AddBoolToObject(resp, "applied", effect != CFG_EFFECT_REBOOT);
    cJSON_AddBoolToObject(resp, "restart_required", effect == CFG_EFFECT_REBOOT);
    cJSON_AddStringToObject(resp, "rollback", "none");
    if (effect == CFG_EFFECT_REBOOT) {
        cJSON_AddStringToObject(resp, "operation_type", "reboot");
    }
}

/* patch 执行(统一层):err_out 置静态错误串或 NULL;changed/effect 输出 */
static bool app_cmd_run_patch(const cJSON *patch, cJSON **changed_out, cfg_effect_t *effect_out,
                              const char **err_out)
{
    cJSON *changed = NULL;
    cfg_effect_t effect = CFG_EFFECT_NONE;
    esp_err_t err = config_apply_patch(patch, &changed, &effect);

    if (err == ESP_OK) {
        *changed_out = changed;
        *effect_out = effect;
        *err_out = NULL;
        return true;
    }
    if (changed != NULL) {
        cJSON_Delete(changed);
    }
    *changed_out = NULL;
    if (err == ESP_ERR_NOT_SUPPORTED) {
        *err_out = "domain_not_supported";
    } else if (err == ESP_ERR_INVALID_ARG) {
        *err_out = "invalid_config";
    } else {
        *err_out = "save_failed";
    }
    return false;
}

/* 与 Web network PUT 一致:body 中显式给出非空 sta_ssid 时,把生效后的
 * SSID/口令固化为一个已保存 profile。config_apply 只负责主配置。 */
static void app_cmd_save_profile_from_params(const cJSON *params)
{
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(params, "sta_ssid");
    esp_err_t err;

    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
        ssid_item->valuestring[0] == '\0') {
        return;
    }
    app_config_lock();
    err = app_config_save_wifi_profile(s_config.sta_ssid, s_config.sta_password);
    app_config_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi profile save failed: %s", esp_err_to_name(err));
    }
}

static app_io_source_t app_cmd_io_source(const app_cmd_context_t *ctx)
{
    /* 内部派生执行按 origin 归属 IO 事件(与改造前"硬编码入口 source"等价);
     * 入口执行按 source。 */
    app_cmd_source_t s = (ctx->exec_env == APP_CMD_EXEC_INTERNAL) ? ctx->origin
                                                                  : ctx->source;
    switch (s) {
    case APP_CMD_SOURCE_MQTT:
        return APP_IO_SOURCE_MQTT;
    case APP_CMD_SOURCE_BLE:
        return APP_IO_SOURCE_BLE;
    case APP_CMD_SOURCE_HTTP:
        return APP_IO_SOURCE_HTTP;
    case APP_CMD_SOURCE_TOUCH:
    default:
        return APP_IO_SOURCE_INPUT;
    }
}

bool app_cmd_net_tryconnect_adapter(const app_cmd_context_t *ctx,
                                    const cJSON *params,
                                    cJSON **resp_out, const char **err_out)
{
    return app_cmd_net_tryconnect_async(params, ctx, resp_out, err_out);
}

bool app_cmd_net_apply_adapter(const app_cmd_context_t *ctx,
                               const cJSON *params,
                               cJSON **resp_out, const char **err_out)
{
    return app_cmd_net_apply_async(ctx, params, resp_out, err_out);
}

bool app_cmd_net_cancel_adapter(const app_cmd_context_t *ctx,
                                const cJSON *params,
                                cJSON **resp_out, const char **err_out)
{
    return app_cmd_net_cancel_async(ctx, params, resp_out, err_out);
}

#ifdef APP_WIFI_FAULT_INJECT
/* M1 测试构建专用入口(默认 OFF;与 APP_C1_TEST_HOOKS 完全独立)。
 * 只注入故障/记录时序,不改变正常路径行为。 */
static void app_cmd_m1hook(const cJSON *params, app_cmd_source_t source,
                           cJSON **resp_out, const char **err_out)
{
    const cJSON *action = cJSON_IsObject(params)
                              ? cJSON_GetObjectItemCaseSensitive(params, "action") : NULL;
    const cJSON *stage = cJSON_GetObjectItemCaseSensitive(params, "stage");
    const cJSON *count_item = cJSON_GetObjectItemCaseSensitive(params, "count");
    const cJSON *timeout_item = cJSON_GetObjectItemCaseSensitive(params, "timeout_ms");
    const char *stage_str = cJSON_IsString(stage) ? stage->valuestring : NULL;
    int count = cJSON_IsNumber(count_item) ? count_item->valueint : 1;
    int timeout_ms = cJSON_IsNumber(timeout_item) ? timeout_item->valueint : 0;
    esp_err_t hook_err;

    if (source != APP_CMD_SOURCE_BLE) {
        *err_out = "guard_rejected";
        return;
    }
    if (!cJSON_IsString(action) || action->valuestring == NULL) {
        *err_out = "params_required";
        return;
    }
#ifdef APP_WIFI_FAULT_INJECT
    if (strcmp(action->valuestring, "op_ttl") == 0) {
        if (timeout_ms < APP_CMD_OP_TTL_MIN_MS || timeout_ms > APP_CMD_OP_TTL_MAX_MS) {
            *err_out = "invalid_param";
            return;
        }
        s_op_ttl_override_ms = timeout_ms;
        ESP_LOGW(TAG, "op_ttl override value_ms=%d (one-shot)", timeout_ms);
        *resp_out = cJSON_CreateObject();
        if (*resp_out == NULL) {
            *err_out = "out_of_memory";
            return;
        }
        cJSON_AddBoolToObject(*resp_out, "ok", true);
        cJSON_AddStringToObject(*resp_out, "action", "op_ttl");
        cJSON_AddNumberToObject(*resp_out, "value_ms", timeout_ms);
        return;
    }
#endif

    hook_err = app_wifi_test_hook(action->valuestring, stage_str, count, timeout_ms);
    if (hook_err == ESP_ERR_INVALID_STATE) {
        *err_out = "busy";
        return;
    }
    if (hook_err != ESP_OK) {
        *err_out = "invalid_param";
        return;
    }
    *resp_out = cJSON_CreateObject();
    if (*resp_out == NULL) {
        *err_out = "out_of_memory";
        return;
    }
    cJSON_AddBoolToObject(*resp_out, "ok", true);
    cJSON_AddStringToObject(*resp_out, "action", action->valuestring);
}
#endif

esp_err_t app_cmd_execute_method(const app_cmd_context_t *ctx, const char *method,
                                 const cJSON *params, cJSON **resp_out,
                                 app_cmd_result_t *out, const char **err_msg)
{
    const char *err = NULL;
    cJSON *resp = NULL;
    bool reboot_after_response = false;
    bool changed_any = false;
    bool handled = false;
    cfg_effect_t effect = CFG_EFFECT_NONE;
    cJSON *changed_list = NULL;

    if (resp_out != NULL) {
        *resp_out = NULL;
    }
    if (err_msg != NULL) {
        *err_msg = NULL;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    /* C1 阶段 1:来源不可信(UNKNOWN/不变量破坏/NULL)一律拒绝,不执行。 */
    if (ctx == NULL || !app_cmd_ctx_valid(ctx)) {
        err = "internal_error";
        goto done;
    }
    /* 阶段 1 保持既有分支逐字不变:全部读 source(内部派生时 source=INTERNAL,
     * 仅执行的 network.apply 走 trial 校验路径,与改造前硬编码入口等价)。 */
    const app_cmd_source_t source = ctx->source;
    app_io_source_t io_source = app_cmd_io_source(ctx);
    if (method == NULL || method[0] == '\0') {
        err = "unknown_method";
        goto done;
    }

    {
        const char *m = method;
        app_cmd_policy_result_t policy = app_cmd_policy_check_network(ctx, m);

        if (policy != APP_CMD_POLICY_ALLOW) {
            switch (policy) {
            case APP_CMD_POLICY_BUSY:            err = "busy"; break;
            case APP_CMD_POLICY_INVALID_ARGUMENT: err = "invalid_argument"; break;
            case APP_CMD_POLICY_FORBIDDEN:
            case APP_CMD_POLICY_NOT_ENABLED:
            default:                             err = "guard_rejected"; break;
            }
            goto done;
        }

#ifdef APP_WIFI_FAULT_INJECT
        if (strcmp(m, "diag.m1hook") == 0) {
            app_cmd_m1hook(params, source, &resp, &err);
            handled = true;
            goto done;
        }
#endif

        handled = true;
        if (strcmp(m, "config.set") == 0) {
            if (!cJSON_IsObject(params)) {
                err = "params_required";
            } else if (!app_cmd_patch_allowed(source, params)) {
                err = "guard_rejected";
            } else if (!app_cmd_cfg_txn_guard_enter()) {
                err = "busy";
            } else {
                if (app_cmd_run_patch(params, &changed_list, &effect, &err)) {
                    changed_any = (changed_list != NULL && cJSON_GetArraySize(changed_list) > 0);
                    app_cmd_save_profile_from_params(params);
                    if (changed_any && effect == CFG_EFFECT_REBOOT) {
                        reboot_after_response = true;
                    }
                    resp = app_cmd_build_success(changed_list, effect);
                    changed_list = NULL;   /* 所有权移交 resp */
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        app_cmd_add_txn_fields(resp, effect);
                    }
                }
                app_cmd_cfg_txn_guard_exit();
            }
        } else if (strcmp(m, "device.info") == 0) {
            resp = app_cmd_build_device_info();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "device.status") == 0) {
            resp = app_cmd_build_device_status();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "config.get") == 0) {
            const cJSON *domains = cJSON_IsObject(params)
                                       ? cJSON_GetObjectItemCaseSensitive(params, "domains")
                                       : NULL;

            if (domains != NULL) {
                const cJSON *item = NULL;

                if (!cJSON_IsArray(domains)) {
                    err = "invalid_config";
                } else {
                    cJSON_ArrayForEach(item, domains) {
                        if (!cJSON_IsString(item) || item->valuestring == NULL) {
                            err = "invalid_config";
                            break;
                        }
                        if (!app_cmd_domain_known(item->valuestring)) {
                            err = "domain_not_supported";
                            break;
                        }
                    }
                }
            }
            if (err == NULL) {
                resp = app_cmd_build_get_config(domains);
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
        } else if (strcmp(m, "io.get") == 0) {
            uint8_t net_mode;
            bool sta_has_ip;
            char sta_ip[sizeof(s_wifi.sta_ip)];
            app_io_state_t io;

            resp = cJSON_CreateObject();
            if (resp == NULL) {
                err = "out_of_memory";
            } else {
                app_config_lock();
                net_mode = s_config.net_mode;
                app_config_unlock();
                app_wifi_runtime_lock();
                sta_has_ip = s_wifi.sta_has_ip;
                memcpy(sta_ip, s_wifi.sta_ip, sizeof(sta_ip));
                app_wifi_runtime_unlock();
                app_io_get_state(&io);
                cJSON_AddBoolToObject(resp, "relay_on", io.relay_on);
                cJSON_AddBoolToObject(resp, "led_on", io.led_on);
                cJSON_AddBoolToObject(resp, "input_active", io.input_active);
                cJSON_AddNumberToObject(resp, "io_seq", io.seq);
                cJSON_AddStringToObject(resp, "io_source", app_io_source_name(io.last_source));
                cJSON_AddStringToObject(resp, "net_mode", app_network_mode_to_string(net_mode));
                cJSON_AddBoolToObject(resp, "sta_has_ip", sta_has_ip);
                cJSON_AddStringToObject(resp, "sta_ip", sta_ip);
            }
        } else if (strcmp(m, "gpio.status") == 0) {
            resp = app_cmd_build_gpio_status();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "uart.runtime") == 0) {
            resp = app_cmd_build_uart_runtime();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "led.toggle") == 0 || strcmp(m, "relay.toggle") == 0) {
            bool is_relay = (strcmp(m, "relay.toggle") == 0);
            app_io_state_t before;
            app_io_state_t after;
            bool want;
            cJSON *changed = cJSON_CreateArray();

            app_io_get_state(&before);
            want = is_relay ? !before.relay_on : !before.led_on;
            if (changed == NULL) {
                err = "out_of_memory";
            } else if (app_io_set_outputs(is_relay, want, !is_relay, want,
                                          io_source) != ESP_OK) {
                cJSON_Delete(changed);
                err = "gpio_apply_failed";
            } else {
                app_io_get_state(&after);
                changed_any = is_relay ? (before.relay_on != after.relay_on)
                                       : (before.led_on != after.led_on);
                if (changed_any) {
                    cJSON_AddItemToArray(changed, cJSON_CreateString(is_relay ? "relay_on" : "led_on"));
                }
                resp = app_cmd_build_success(changed, CFG_EFFECT_NONE);
                changed = NULL;
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
            if (changed != NULL) {
                cJSON_Delete(changed);
            }
        } else if (strcmp(m, "relay.set") == 0 || strcmp(m, "led.set") == 0) {
            bool is_relay = (strcmp(m, "relay.set") == 0);
            app_io_state_t before;
            app_io_state_t after;
            bool want;
            cJSON *changed = cJSON_CreateArray();

            app_io_get_state(&before);
            want = app_cmd_parse_bool_param(params, !(is_relay ? before.relay_on : before.led_on));
            if (changed == NULL) {
                err = "out_of_memory";
            } else if (app_io_set_outputs(is_relay, want, !is_relay, want,
                                          io_source) != ESP_OK) {
                cJSON_Delete(changed);
                err = "gpio_apply_failed";
            } else {
                app_io_get_state(&after);
                changed_any = is_relay ? (before.relay_on != after.relay_on)
                                       : (before.led_on != after.led_on);
                if (changed_any) {
                    cJSON_AddItemToArray(changed, cJSON_CreateString(is_relay ? "relay_on" : "led_on"));
                }
                resp = app_cmd_build_success(changed, CFG_EFFECT_NONE);
                changed = NULL;
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
            if (changed != NULL) {
                cJSON_Delete(changed);
            }
        } else if (strcmp(m, "io.set") == 0) {
            cJSON *rel = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "relay_on") : NULL;
            cJSON *led = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "led_on") : NULL;
            bool set_relay = cJSON_IsBool(rel) || cJSON_IsNumber(rel);
            bool set_led = cJSON_IsBool(led) || cJSON_IsNumber(led);
            bool relay_on = set_relay ? (cJSON_IsBool(rel) ? cJSON_IsTrue(rel) : (rel->valueint != 0)) : false;
            bool led_on = set_led ? (cJSON_IsBool(led) ? cJSON_IsTrue(led) : (led->valueint != 0)) : false;
            app_io_state_t before;
            app_io_state_t after;
            cJSON *changed = NULL;

            if (!set_relay && !set_led) {
                err = "params_required";
            } else {
                app_io_get_state(&before);
                changed = cJSON_CreateArray();
                if (changed == NULL) {
                    err = "out_of_memory";
                } else if (app_io_set_outputs(set_relay, relay_on, set_led, led_on,
                                              io_source) != ESP_OK) {
                    cJSON_Delete(changed);
                    changed = NULL;
                    err = "gpio_apply_failed";
                } else {
                    app_io_get_state(&after);
                    changed_any = (before.relay_on != after.relay_on) ||
                                  (before.led_on != after.led_on);
                    if (before.relay_on != after.relay_on) {
                        cJSON_AddItemToArray(changed, cJSON_CreateString("relay_on"));
                    }
                    if (before.led_on != after.led_on) {
                        cJSON_AddItemToArray(changed, cJSON_CreateString("led_on"));
                    }
                    resp = app_cmd_build_success(changed, CFG_EFFECT_NONE);
                    changed = NULL;
                    if (resp == NULL) {
                        err = "out_of_memory";
                    }
                }
                if (changed != NULL) {
                    cJSON_Delete(changed);
                }
            }
        } else if (strcmp(m, "auth.admin.set") == 0) {
            const cJSON *old_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "old_password")
                                        : NULL;
            const cJSON *new_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "new_password")
                                        : NULL;
            const cJSON *user_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "new_username")
                                         : NULL;
            const char *new_user = cJSON_IsString(user_item) ? user_item->valuestring : NULL;
            uint8_t salt[16];

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(old_item) || old_item->valuestring == NULL ||
                       !app_cmd_verify_auth_password(old_item->valuestring)) {
                err = "invalid_old_password";
            } else if (!cJSON_IsString(new_item) || new_item->valuestring == NULL ||
                       (strlen(new_item->valuestring) < 8 &&
                        strcmp(new_item->valuestring, CONFIG_AUTH_DEFAULT_PASSWORD) != 0)) {
                err = "password_too_short";
            } else if (new_user != NULL && new_user[0] != '\0' &&
                       strchr(new_user, ':') != NULL) {
                err = "invalid_username";
            } else if (!app_cmd_cfg_txn_guard_enter()) {
                err = "busy";
            } else {
                esp_err_t save_err;

                esp_fill_random(salt, sizeof(salt));
                app_config_lock();
                if (new_user != NULL && new_user[0] != '\0') {
                    app_copy_string(s_config.auth_username, sizeof(s_config.auth_username), new_user);
                }
                memcpy(s_config.auth_pass_salt, salt, sizeof(salt));
                app_auth_hash_password(salt, sizeof(salt), new_item->valuestring,
                                      s_config.auth_pass_hash);
                save_err = app_config_save(&s_config);
                app_config_unlock();
                app_cmd_cfg_txn_guard_exit();
                if (save_err != ESP_OK) {
                    err = "save_failed";
                } else {
                    /* 口令哈希已落盘成功。先构造成功响应,再作废全部既有
                     * token(含串口槽):发送路径不读 token,清理不会截断响应;
                     * 即使 cJSON 分配失败,改密已生效,仍必须清全部会话。 */
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddBoolToObject(resp, "saved", true);
                        cJSON_AddBoolToObject(resp, "sessions_invalidated", true);
                    }
                    app_token_store_clear_all();
                }
            }
        } else if (strcmp(m, "provisioning.pin.set") == 0) {
            const cJSON *pin_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "pin")
                                        : NULL;
            const cJSON *pass_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "password")
                                         : NULL;
            esp_err_t save_err;

            /* 配网 PIN 属于敏感操作：只允许本地 HTTP/BLE，且要求当前管理员口令。
             * 不回显 PIN，不写日志；新值在下次进入配网时生效。 */
            if (source != APP_CMD_SOURCE_HTTP && source != APP_CMD_SOURCE_BLE) {
                err = "forbidden";
            } else if (!cJSON_IsString(pin_item) || pin_item->valuestring == NULL ||
                       !app_prov_pin_is_valid(pin_item->valuestring)) {
                err = "invalid_param";
            } else if (!cJSON_IsString(pass_item) || pass_item->valuestring == NULL ||
                       !app_cmd_verify_auth_password(pass_item->valuestring)) {
                err = "unauthorized";
            } else {
                save_err = app_prov_pin_set(pin_item->valuestring);
                if (save_err != ESP_OK) {
                    err = "save_failed";
                } else {
                    cJSON *changed;

                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddBoolToObject(resp, "saved", true);
                        cJSON_AddBoolToObject(resp, "applied", true);
                        cJSON_AddBoolToObject(resp, "restart_required", false);
                        cJSON_AddStringToObject(resp, "effect", "none");
                        cJSON_AddBoolToObject(resp, "pin_set", true);
                        changed = cJSON_AddArrayToObject(resp, "changed");
                        if (changed != NULL) {
                            cJSON_AddItemToArray(changed,
                                                 cJSON_CreateString("provisioning_pin"));
                        }
                    }
                }
            }
        } else if (strcmp(m, "uart.send") == 0) {
            const cJSON *enc_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "encoding")
                                        : NULL;
            const cJSON *data_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "data")
                                         : NULL;
            const char *encoding = cJSON_IsString(enc_item) ? enc_item->valuestring : "text";
            unsigned sent = 0;
            esp_err_t uart_err;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(data_item) || data_item->valuestring == NULL) {
                err = "params_required";
            } else {
                uart_err = app_uart_send_data(encoding, data_item->valuestring, &sent);
                if (uart_err != ESP_OK) {
                    err = "uart_send_failed";
                } else {
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddNumberToObject(resp, "sent", (double)sent);
                    }
                }
            }
        } else if (strcmp(m, "data.loopback") == 0) {
            const cJSON *enabled_item = cJSON_IsObject(params)
                                            ? cJSON_GetObjectItemCaseSensitive(params, "enabled")
                                            : NULL;
            bool enabled;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsBool(enabled_item) && !cJSON_IsNumber(enabled_item)) {
                err = "params_required";
            } else {
                enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item)
                                                      : (enabled_item->valueint != 0);
                data_pipe_set_loopback(enabled);
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddBoolToObject(resp, "loopback", enabled);
                }
            }
        } else if (strcmp(m, "network.status") == 0) {
            resp = app_cmd_build_network_status();
            if (resp == NULL) {
                err = "out_of_memory";
            }
#ifdef APP_C1_TEST_HOOKS
        } else if (strcmp(m, "diag.testhook") == 0) {
            /* 实验构建专用:仅 BLE 来源。arm 携带可选 probe_to("got_ip" 等);
             * try 对 operation_id 直调 set_state 验证 expiring 闸。 */
            const cJSON *action = cJSON_IsObject(params)
                                      ? cJSON_GetObjectItemCaseSensitive(params, "action") : NULL;
            if (source != APP_CMD_SOURCE_BLE) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(action) || action->valuestring == NULL) {
                err = "params_required";
            } else if (strcmp(action->valuestring, "arm") == 0) {
                const cJSON *probe = cJSON_GetObjectItemCaseSensitive(params, "probe_to");
                const cJSON *op_item = cJSON_GetObjectItemCaseSensitive(params,
                                                                       "operation_id");
                int probe_state = 0;

                if (op_item != NULL && !cJSON_IsString(op_item)) {
                    err = "invalid_param";
                }
                if (cJSON_IsString(probe) && probe->valuestring != NULL) {
                    if (strcmp(probe->valuestring, "got_ip") == 0) probe_state = (int)APP_OP_GOT_IP;
                    else if (strcmp(probe->valuestring, "committing") == 0) probe_state = (int)APP_OP_COMMITTING;
                    else if (strcmp(probe->valuestring, "failed") == 0) probe_state = (int)APP_OP_FAILED;
                    else { err = "invalid_param"; }
                }
                if (err == NULL && op_item != NULL) {
#ifdef APP_C15_HOOK_TARGET
                    const cJSON *hold = cJSON_GetObjectItemCaseSensitive(params, "hold_ms");
                    uint32_t hold_ms = 310000;
                    uint32_t generation = 0;
                    int64_t deadline_ms = 0;
                    esp_err_t arm_err;

                    if (cJSON_IsNumber(hold)) {
                        if (hold->valueint < 100 || hold->valueint > 360000) {
                            err = "invalid_param";
                        } else {
                            hold_ms = (uint32_t)hold->valueint;
                        }
                    }
                    if (err == NULL) {
                        arm_err = app_operation_test_hook_arm_target(
                            op_item->valuestring, hold_ms, probe_state,
                            &generation, &deadline_ms);
                        if (arm_err == ESP_ERR_NOT_FOUND) {
                            err = "not_found";
                        } else if (arm_err == ESP_ERR_INVALID_STATE) {
                            err = "not_active";
                        } else if (arm_err == ESP_ERR_INVALID_ARG) {
                            err = "invalid_param";
                        } else if (arm_err != ESP_OK) {
                            err = "internal_error";
                        } else {
                            int64_t now_ms = esp_timer_get_time() / 1000;
                            int64_t remaining_ms = deadline_ms > now_ms
                                                       ? deadline_ms - now_ms : 0;

                            resp = cJSON_CreateObject();
                            if (resp != NULL) {
                                cJSON_AddBoolToObject(resp, "armed", true);
                                cJSON_AddNumberToObject(resp, "generation",
                                                        (double)generation);
                                cJSON_AddNumberToObject(resp, "deadline_ms",
                                                        (double)deadline_ms);
                                cJSON_AddNumberToObject(resp, "remaining_ms",
                                                        (double)remaining_ms);
                                cJSON_AddNumberToObject(resp, "hold_ms",
                                                        (double)hold_ms);
                            }
                        }
                    }
#else
                    err = "not_active";
#endif
                } else if (err == NULL) {
                    const cJSON *hold = cJSON_GetObjectItemCaseSensitive(params, "hold_ms");
                    uint32_t hold_ms = 8000;

                    if (cJSON_IsNumber(hold) && hold->valueint >= 100 &&
                        hold->valueint <= 30000) {
                        hold_ms = (uint32_t)hold->valueint;
                    }
                    app_operation_test_hook_arm(hold_ms, probe_state);
                    resp = cJSON_CreateObject();
                    if (resp != NULL) {
                        cJSON_AddBoolToObject(resp, "armed", true);
                        cJSON_AddNumberToObject(resp, "hold_ms", (double)hold_ms);
                    }
                }
            } else if (strcmp(action->valuestring, "wifi_event") == 0) {
                /* 实验构建专用：直接注入 Wi-Fi 事件，确定性验证
                 * tryconnect 的旧 STA_CONNECTED/GOT_IP 归属边界。 */
                const cJSON *kind_item = cJSON_GetObjectItemCaseSensitive(params, "event");
                const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(params, "ssid");
                const cJSON *bssid_item = cJSON_GetObjectItemCaseSensitive(params, "bssid");
                const cJSON *reason_item = cJSON_GetObjectItemCaseSensitive(params, "reason");
                esp_err_t hook_err;

                if (source != APP_CMD_SOURCE_BLE) {
                    err = "guard_rejected";
                } else if (!cJSON_IsString(kind_item) || kind_item->valuestring == NULL) {
                    err = "params_required";
                } else {
                    hook_err = app_wifi_test_inject_event(
                        kind_item->valuestring,
                        cJSON_IsString(ssid_item) ? ssid_item->valuestring : NULL,
                        cJSON_IsString(bssid_item) ? bssid_item->valuestring : NULL,
                        cJSON_IsNumber(reason_item) ? reason_item->valueint : 0);
                    if (hook_err == ESP_ERR_INVALID_STATE) {
                        err = "not_active";
                    } else if (hook_err != ESP_OK) {
                        err = "invalid_param";
                    } else {
                        resp = cJSON_CreateObject();
                        if (resp != NULL) {
                            cJSON_AddBoolToObject(resp, "injected", true);
                            cJSON_AddStringToObject(resp, "event", kind_item->valuestring);
                        }
                    }
                }
            } else if (strcmp(action->valuestring, "clear") == 0) {
                app_operation_test_hook_clear();
                resp = cJSON_CreateObject();
                if (resp != NULL) cJSON_AddBoolToObject(resp, "armed", false);
            } else if (strcmp(action->valuestring, "try") == 0) {
                const cJSON *op_item = cJSON_GetObjectItemCaseSensitive(params, "operation_id");
                const cJSON *to_item = cJSON_GetObjectItemCaseSensitive(params, "to");
                app_operation_t *op;
                app_op_state_t to;
                esp_err_t s_err;

                if (!cJSON_IsString(op_item) || !cJSON_IsString(to_item)) {
                    err = "params_required";
                } else {
                    op = app_operation_find(op_item->valuestring);
                    to = (strcmp(to_item->valuestring, "got_ip") == 0) ? APP_OP_GOT_IP
                       : (strcmp(to_item->valuestring, "committing") == 0) ? APP_OP_COMMITTING
                       : (strcmp(to_item->valuestring, "failed") == 0) ? APP_OP_FAILED
                       : APP_OP_STATE_COUNT;
                    if (op == NULL) {
                        err = "not_found";
                    } else if (to == APP_OP_STATE_COUNT) {
                        err = "invalid_param";
                    } else {
                        s_err = app_operation_set_state(op, to, APP_OPERATION_ERR_NONE);
                        resp = cJSON_CreateObject();
                        if (resp != NULL) {
                            cJSON_AddBoolToObject(resp, "rejected", s_err != ESP_OK);
                            cJSON_AddStringToObject(resp, "result", esp_err_to_name(s_err));
                        }
                    }
                }
            } else {
                err = "unknown_command";
            }
#endif
        } else if (strcmp(m, "operation.get") == 0) {
            const cJSON *op_item = cJSON_IsObject(params)
                                       ? cJSON_GetObjectItemCaseSensitive(params, "operation_id")
                                       : NULL;
            if (!cJSON_IsString(op_item) || op_item->valuestring == NULL) {
                err = "params_required";
            } else {
                resp = app_cmd_net_operation_status(ctx, op_item->valuestring, &err);
            }
        } else if (strcmp(m, "network.tryconnect") == 0) {
            const cJSON *ssid_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "ssid")
                                         : NULL;
            const cJSON *pass_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "password")
                                         : NULL;
            app_wifi_try_result_t result;
            const char *ssid;
            const char *password;
            app_cmd_cfg_txn_t cfg_txn = {0};
            bool cfg_txn_valid = false;
            bool cfg_txn_transferred = false;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
                       ssid_item->valuestring[0] == '\0') {
                err = "params_required";
            } else if (app_cmd_static_switch_blocked(ssid_item->valuestring, params, &err)) {
                /* err set by helper */
            } else if (app_cmd_net_job_is_active()) {
                err = "busy";
            } else {
                /* R8:trial 忙检/清理入锁;紧随其后的 15s 阻塞试连不持锁
                 * (断连期间到达的清理会在 store_trial 锁内与其串行)。 */
                app_cmd_net_job_lock();
                if (app_cmd_net_trial_busy_locked()) {
                    app_cmd_net_job_unlock();
                    err = "busy";
                } else {
                    app_cmd_net_trial_clear_locked();
                    app_cmd_net_job_unlock();
                    if (!app_cmd_cfg_txn_begin(&cfg_txn)) {
                        err = "busy";
                    } else {
                        cfg_txn_valid = true;
                    ssid = ssid_item->valuestring;
                password = cJSON_IsString(pass_item) && pass_item->valuestring != NULL
                               ? pass_item->valuestring : "";
                ESP_LOGI(TAG, "network.tryconnect ssid='%s'", ssid);
                result = app_wifi_try_connect_ip(ssid, password, 15000);
                switch (result) {
                case APP_WIFI_TRY_OK: {
                    /* tryconnect 是同步阻塞的:等待期间 BLE 可能已断开。
                     * 此时若仍把候选留在 RAM,会形成"无会话但可 apply"的遗留事务;
                     * 直接丢弃候选,连接已恢复的正式配置不受影响。 */
                    bool txn_taken = false;

                    if (app_cmd_net_trial_store(ssid, password, source, NULL, &cfg_txn, NULL,
                                                &txn_taken) != APP_CMD_NET_TRIAL_STORED) {
                        ESP_LOGW(TAG, "network.tryconnect: BLE disconnected, discard candidate");
                        err = "cancelled";
                        break;
                    }
                    cfg_txn_transferred = txn_taken;
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        app_cmd_net_trial_clear();
                        err = "out_of_memory";
                    } else {
                        /* R8:锁内值复制(防中途断连清理撕读)——
                         * CMD-04c:构建移到锁外(锁内零 cJSON)。 */
                        char trial_id[APP_CMD_NET_TRIAL_ID_LEN];
                        char trial_ssid[APP_CMD_NET_TRIAL_SSID_LEN];
                        app_cmd_net_trial_snap_t trial_snap;

                        app_cmd_net_job_lock();
                        app_cmd_net_trial_snapshot_locked(&trial_snap);
                        app_cmd_net_job_unlock();
                        app_copy_string(trial_id, sizeof(trial_id), trial_snap.id);
                        app_copy_string(trial_ssid, sizeof(trial_ssid), trial_snap.ssid);
                        cJSON_AddStringToObject(resp, "trial_id", trial_id);
                        cJSON_AddStringToObject(resp, "state", "got_ip");
                        cJSON_AddStringToObject(resp, "ssid", trial_ssid);
                    }
                    break;
                }
                case APP_WIFI_TRY_WRONG_PASSWORD:
                    err = "wrong_password";
                    break;
                case APP_WIFI_TRY_AP_NOT_FOUND:
                    err = "ap_not_found";
                    break;
                case APP_WIFI_TRY_SCAN_FAILED:
                    err = "wifi_scan_failed";
                    break;
                case APP_WIFI_TRY_TIMEOUT:
                    err = "connect_timeout";
                    break;
                case APP_WIFI_TRY_BUSY:
                    err = "busy";
                    break;
                case APP_WIFI_TRY_INVALID_ARG:
                    err = "invalid_param";
                    break;
                case APP_WIFI_TRY_CONFIG_OFFLINE:
                    /* M1 R9:离线门控拒绝(v0 字符串) */
                    err = "wifi_config_offline";
                    break;
                default:
                    err = "connect_failed";
                    break;
                }
                if (cfg_txn_valid && !cfg_txn_transferred) {
                    (void)app_cmd_cfg_txn_end(cfg_txn);
                }
                    }
                }
            }
        } else if (strcmp(m, "network.apply") == 0) {
            const cJSON *trial_item = cJSON_IsObject(params)
                                          ? cJSON_GetObjectItemCaseSensitive(params, "trial_id")
                                          : NULL;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (app_cmd_net_job_is_active() && !app_cmd_net_job_apply_internal()) {
                err = "busy";
            } else if (!cJSON_IsString(trial_item) || trial_item->valuestring == NULL ||
                       !app_cmd_net_trial_ready(trial_item->valuestring)) {
                err = "trial_invalid";
            } else if (app_config_snapshot_save() != ESP_OK) {
                app_cmd_net_trial_clear();
                err = "storage_failed";
            } else {
                cJSON *patch = cJSON_CreateObject();
                cJSON *changed = NULL;
                cfg_effect_t effect = CFG_EFFECT_NONE;
                esp_err_t apply_err = ESP_FAIL;
                /* CMD-04c:trial 字段栈副本——零初始化(评审检查点 1:
                 * patch OOM 路径擦除时也有确定内容)。 */
                char trial_ssid[33] = {0};
                char trial_password[65] = {0};

                if (patch != NULL) {
                    /* R8:trial 字段读点入锁(...)。
                     * CMD-04c:锁内值复制——patch 构建移到锁外(锁内零 cJSON)。 */
                    app_cmd_net_job_lock();
                    (void)app_cmd_net_trial_credentials_locked(
                        trial_ssid, sizeof(trial_ssid),
                        trial_password, sizeof(trial_password));
                    app_cmd_net_job_unlock();
                    /* CMD-04b 评审修正的恢复:候选 SSID/密码从快照副本加入
                     * patch——字段顺序(1.sta_ssid 2.sta_password 3.net_mode)
                     * 与现状一致。此前 969416e 遗漏了这两行,导致
                     * config_apply_patch 收到无候选凭据的空 patch(P0)。 */
                    cJSON_AddStringToObject(patch, "sta_ssid", trial_ssid);
                    cJSON_AddStringToObject(patch, "sta_password", trial_password);
                    /* BLE-only 配网：提交必须是 STA，不能把 AP-only 旧配置推断成 APSTA。
                     * AP/Web 正在退出，apply 后不应继续保留 AP 的常驻内存开销。 */
                    cJSON_AddStringToObject(patch, "net_mode", "sta");
                    /* Static mode is pinned to the formal SSID; keep it when the
                     * trial target matches. DHCP candidates already have
                     * sta_use_static=false, so no forced rewrite is needed. */
                    apply_err = config_apply_patch(patch, &changed, &effect);
                    /* CMD-04c 擦除点(评审要求):config_apply_patch 返回后、
                     * 删除前——擦除 patch 堆副本中的明文(空指针保护：
                     * AddString 失败时节点/valuestring 可能为空)。
                     * 成功/失败两路径共用本擦除点。 */
                    {
                        cJSON *pw_item =
                            cJSON_GetObjectItemCaseSensitive(patch, "sta_password");

                        if (pw_item != NULL && cJSON_IsString(pw_item) &&
                            pw_item->valuestring != NULL) {
                            app_cmd_secure_wipe(pw_item->valuestring,
                                                strlen(pw_item->valuestring) + 1);
                        }
                    }
                    cJSON_Delete(patch);
                    cJSON_Delete(changed);
                }
                /* CMD-04c:栈副本擦除——成败/OOM 三路径汇合（评审检查点 1：
                 * 局部缓冲零初始化，patch OOM 路径擦除内容确定）。 */
                app_cmd_secure_wipe(trial_password, sizeof(trial_password));
                if (apply_err != ESP_OK) {
                    app_config_snapshot_discard();
                    app_cmd_net_trial_clear();
                    err = apply_err == ESP_ERR_INVALID_ARG ? "invalid_config" : "save_failed";
                } else {
                    char prof_ssid[APP_CMD_NET_TRIAL_SSID_LEN] = {0};
                    char prof_password[APP_CMD_NET_TRIAL_PASSWORD_LEN] = {0};
                    esp_err_t profile_err;

                    app_cmd_net_job_lock();
                    (void)app_cmd_net_trial_credentials_locked(
                        prof_ssid, sizeof(prof_ssid),
                        prof_password, sizeof(prof_password));
                    profile_err = app_config_save_wifi_profile(prof_ssid, prof_password);
                    app_cmd_net_job_unlock();
                    app_cmd_secure_wipe(prof_password, sizeof(prof_password));

                    if (profile_err != ESP_OK) {
                        esp_err_t restore_err = app_config_snapshot_restore();

                        app_cmd_net_trial_clear();
                        err = restore_err == ESP_OK ? "storage_failed" : "rollback_failed";
                    } else if (!app_wifi_schedule_restart()) {
                        esp_err_t restore_err = app_config_snapshot_restore();

                        app_cmd_net_trial_clear();
                        err = restore_err == ESP_OK ? "apply_failed" : "rollback_failed";
                    } else {
                        char trial_id[APP_CMD_NET_TRIAL_ID_LEN];
                        app_cmd_net_trial_snap_t trial_snap;

                        app_cmd_net_job_lock();
                        app_cmd_net_trial_snapshot_locked(&trial_snap);
                        app_cmd_net_job_unlock();
                        snprintf(trial_id, sizeof(trial_id), "%s", trial_snap.id);
                        app_config_snapshot_discard();
                        app_cmd_net_trial_clear();
                        resp = cJSON_CreateObject();
                        if (resp == NULL) {
                            err = "out_of_memory";
                        } else {
                            cJSON_AddStringToObject(resp, "trial_id", trial_id);
                            cJSON_AddStringToObject(resp, "state", "success");
                            cJSON_AddBoolToObject(resp, "saved", true);
                            cJSON_AddBoolToObject(resp, "applied", true);
                        }
                    }
                }
            }
        } else if (strcmp(m, "network.cancel") == 0) {
            const cJSON *trial_item = cJSON_IsObject(params)
                                          ? cJSON_GetObjectItemCaseSensitive(params, "trial_id")
                                          : NULL;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (app_cmd_net_job_is_active()) {
                err = "busy";
            } else if (!cJSON_IsString(trial_item) || trial_item->valuestring == NULL ||
                       !app_cmd_net_trial_ready(trial_item->valuestring)) {
                err = "not_found";
            } else {
                char trial_id[APP_CMD_NET_TRIAL_ID_LEN];
                app_cmd_net_trial_snap_t trial_snap;

                app_cmd_net_job_lock();
                app_cmd_net_trial_snapshot_locked(&trial_snap);
                app_cmd_net_job_unlock();
                snprintf(trial_id, sizeof(trial_id), "%s", trial_snap.id);
                app_cmd_net_trial_clear();
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddStringToObject(resp, "trial_id", trial_id);
                    cJSON_AddStringToObject(resp, "state", "cancelled");
                }
            }
        } else if (strcmp(m, "network.profile.list") == 0) {
            app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX] = {0};
            size_t profile_count = 0;
            cJSON *arr;

            if (app_config_load_wifi_profiles(profiles, APP_WIFI_PROFILE_MAX,
                                              &profile_count) != ESP_OK) {
                profile_count = 0;
            }
            resp = cJSON_CreateObject();
            if (resp == NULL) {
                err = "out_of_memory";
            } else {
                arr = cJSON_AddArrayToObject(resp, "profiles");
                if (arr != NULL) {
                    size_t i;

                    for (i = 0; i < profile_count; i++) {
                        cJSON *item = cJSON_CreateObject();

                        if (item == NULL) {
                            continue;
                        }
                        cJSON_AddStringToObject(item, "ssid", profiles[i].ssid);
                        cJSON_AddBoolToObject(item, "password_set",
                                              profiles[i].password[0] != '\0');
                        cJSON_AddItemToArray(arr, item);
                    }
                }
            }
        } else if (strcmp(m, "network.scan") == 0) {
            cJSON *arr;
            uint16_t scan_count;
            uint16_t index;
            bool scan_owner_busy = false;
            esp_err_t scan_rc;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!app_cmd_scan_gate_check()) {
                /* DEF-SCAN-001:距上次扫描结束不足 3s,直接拒绝(射频需空闲) */
                err = "busy";
            } else if ((scan_rc = app_wifi_perform_scan_command_ex(&scan_owner_busy)) != ESP_OK) {
                if (scan_owner_busy) {
                    /* P3:owner 忙是运态拒绝,回 conflict(1s);不占用 3s 扫描节流窗 */
                    err = "conflict";
                } else {
                    (void)scan_rc;
                    app_cmd_scan_gate_stamp();   /* 失败同样占用过射频 */
                    err = "wifi_scan_failed";
                }
            } else {
                app_cmd_scan_gate_stamp();
                app_wifi_runtime_lock();
                scan_count = s_wifi.scan_count;
                app_wifi_runtime_unlock();
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddStringToObject(resp, "status", "ok");
                    cJSON_AddNumberToObject(resp, "count", (double)scan_count);
                    arr = cJSON_AddArrayToObject(resp, "aps");
                    if (arr != NULL) {
                        for (index = 0; index < scan_count; index++) {
                            cJSON *ap = cJSON_CreateObject();
                            const char *ssid = "";
                            int8_t rssi = 0;
                            uint8_t authmode = 0;
                            char bssid[18];

                            if (ap == NULL) {
                                continue;
                            }
                            if (!app_wifi_scan_record_at(index, &ssid, &rssi, &authmode)) {
                                cJSON_Delete(ap);
                                continue;
                            }
                            cJSON_AddStringToObject(ap, "ssid", ssid);
                            cJSON_AddNumberToObject(ap, "rssi", (double)rssi);
                            cJSON_AddNumberToObject(ap, "auth", (double)authmode);
                            if (app_wifi_scan_record_bssid(index, bssid, sizeof(bssid))) {
                                cJSON_AddStringToObject(ap, "bssid", bssid);
                            }
                            cJSON_AddItemToArray(arr, ap);
                        }
                    }
                }
            }
        } else if (strcmp(m, "network.profile.delete") == 0) {
            const cJSON *ssid_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "ssid")
                                         : NULL;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
                       ssid_item->valuestring[0] == '\0') {
                err = "params_required";
            } else if (!app_cmd_cfg_txn_guard_enter()) {
                err = "busy";
            } else {
                esp_err_t del_err = app_config_delete_wifi_profile(ssid_item->valuestring);
                app_cmd_cfg_txn_guard_exit();

                if (del_err == ESP_ERR_NOT_FOUND) {
                    err = "not_found";
                } else if (del_err == ESP_ERR_INVALID_STATE) {
                    err = "last_profile";
                } else if (del_err != ESP_OK) {
                    err = "delete_failed";
                } else {
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddStringToObject(resp, "deleted", ssid_item->valuestring);
                    }
                }
            }
        } else if (strcmp(m, "network.reconnect") == 0) {
            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!app_wifi_schedule_restart()) {
                err = "restart_failed";
            } else {
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddBoolToObject(resp, "restarting", true);
                }
            }
        } else {
            err = "unknown_method";
        }
    }

done:
    if (changed_list != NULL) {
        cJSON_Delete(changed_list);
    }
    if (err != NULL && resp != NULL) {
        cJSON_Delete(resp);
        resp = NULL;
    }
    if (resp_out != NULL) {
        *resp_out = resp;
    } else if (resp != NULL) {
        cJSON_Delete(resp);
    }
    if (out != NULL) {
        out->reboot_after_response = reboot_after_response;
        out->changed_any = changed_any;
        out->handled = handled;
    }
    if (err_msg != NULL) {
        *err_msg = err;
    }
    return err != NULL ? ESP_FAIL : ESP_OK;
}
