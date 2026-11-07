#include <errno.h>
#include <stdio.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "esp_log.h"
#include "esp_timer.h"

#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "lwip/sockets.h"

#include "config.h"
#include "mqtt.h"
#include "netcheck.h"
#include "netcheck_policy.h"

static const char *TAG = "netcheck";

/* 仓内约定:config.h 不导出 s_config,各 .c 自行 extern(见 wifi.c:316 等) */
extern app_config_t s_config;

/* 总预算(评审 P1-1):25s,accepted_ttl=45s 覆盖 15s 连接 + 25s 验证。 */
#define APP_NETCHECK_BUDGET_MS 25000
#define APP_NETCHECK_TCP_TIMEOUT_MS 2500
/* 3 次 × 2s 间隔(2026-11-07 TQ2018/18:52 实测定案):拆链重连后路由未就绪
 * 窗口 W 实测 0.5~6s+ 波动,探测点 0.5/3.0/5.6s 覆盖之;总预算 25s 仍兜底 */
#define APP_NETCHECK_TCP_ATTEMPTS 3
#define APP_NETCHECK_TCP_RETRY_GAP_MS 2000
#define APP_NETCHECK_POLL_MS 100
#define APP_NETCHECK_MQTT_GRACE_MS 4000
/* DNS 独立子预算(核验 P3-1):慢解析不得挤占 TCP/MQTT 段,归属不漂移 */
#define APP_NETCHECK_DNS_BUDGET_MS 3000

typedef struct {
    portMUX_TYPE mux;
    app_netcheck_state_t state;
    uint32_t run_gen;
    uint32_t pending_gen; /* 0=无;有在跑的旧代次时排队 */
    bool cancel_req;
    char last_stage[12];
    TaskHandle_t task;
    /* 选 B 条件 4:按代次归档的不可变终局快照——收尾时写入,只读,
     * 直到下一次收尾才覆盖;worker 只读自己代次的快照,不读 live 状态 */
    bool done_valid;
    uint32_t done_gen;
    app_netcheck_outcome_t done_outcome;
    char done_stage[12];
    int64_t done_ms;
} netcheck_state_t;

static netcheck_state_t s_nc = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
    .state = APP_NETCHECK_IDLE,
    .last_stage = "none",
};

/* 异步 DNS 结果承载:回调(tcpip 线程)写,任务侧读,短临界区保可见。 */
static struct {
    portMUX_TYPE mux;
    volatile bool done;
    volatile bool ok;
    ip_addr_t addr;
} s_dns = {
    .mux = portMUX_INITIALIZER_UNLOCKED,
};

static void (*s_observer)(uint32_t gen, bool ok, const char *stage) = NULL;

static int64_t nc_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static const char *nc_state_name(app_netcheck_state_t st)
{
    switch (st) {
    case APP_NETCHECK_IDLE: return "idle";
    case APP_NETCHECK_RUNNING: return "running";
    case APP_NETCHECK_OK: return "ok";
    case APP_NETCHECK_FAIL_INTERNET: return "fail_internet";
    case APP_NETCHECK_FAIL_MQTT: return "fail_mqtt";
    case APP_NETCHECK_CANCELLED: return "cancelled";
    default: return "?";
    }
}

/* ---- 事实收集 ---------------------------------------------------------- */

static bool nc_cancelled_or_superseded(uint32_t gen)
{
    bool stop;

    portENTER_CRITICAL(&s_nc.mux);
    stop = s_nc.cancel_req ||
           (s_nc.pending_gen != 0 && s_nc.pending_gen != gen);
    portEXIT_CRITICAL(&s_nc.mux);
    return stop;
}

static void nc_dns_cb(const char *name, const ip_addr_t *ipaddr, void *arg)
{
    (void)name;
    (void)arg;
    portENTER_CRITICAL(&s_dns.mux);
    if (ipaddr != NULL) {
        s_dns.ok = true;
        ip_addr_copy(s_dns.addr, *ipaddr);
    } else {
        s_dns.ok = false;
    }
    s_dns.done = true;
    portEXIT_CRITICAL(&s_dns.mux);
}

/* 非阻塞 TCP connect + select 等 writable,出结果即关套接字。
 * 最长 timeout_ms,期间不可取消(select 有界)—— 取消收敛以此为准(≤2.5s)。 */
static bool nc_tcp_probe(const ip_addr_t *addr, uint16_t port, uint32_t timeout_ms)
{
    struct sockaddr_in sa = {0};
    int sock;
    int rc;
    bool ok = false;

    if (addr == NULL || !IP_IS_V4(addr)) {
        /* 本固件仅 IPv4;主机名解析出 AAAA 亦按不可达处理(记录在案) */
        return false;
    }
    sock = lwip_socket(AF_INET, SOCK_STREAM, 0);
    if (sock < 0) {
        return false;
    }
    lwip_fcntl(sock, F_SETFL, O_NONBLOCK);
    sa.sin_family = AF_INET;
    sa.sin_port = lwip_htons(port);
    sa.sin_addr.s_addr = ip_2_ip4(addr)->addr;

    rc = lwip_connect(sock, (struct sockaddr *)&sa, sizeof(sa));
    if (rc == 0) {
        ok = true;
    } else if (errno == EINPROGRESS || errno == EWOULDBLOCK) {
        fd_set wset;
        struct timeval tv;
        int sel;

        FD_ZERO(&wset);
        FD_SET(sock, &wset);
        tv.tv_sec = (long)(timeout_ms / 1000);
        tv.tv_usec = (long)((timeout_ms % 1000) * 1000);
        sel = lwip_select(sock + 1, NULL, &wset, NULL, &tv);
        if (sel > 0) {
            int soerr = 0;
            socklen_t len = sizeof(soerr);

            if (lwip_getsockopt(sock, SOL_SOCKET, SO_ERROR, &soerr, &len) == 0) {
                ok = (soerr == 0);
                if (!ok) {
                    ESP_LOGI(TAG, "tcp soerr=%d", soerr);
                }
            }
        } else {
            ESP_LOGI(TAG, "tcp select=%d errno=%d", sel, errno);
        }
    } else {
        ESP_LOGI(TAG, "tcp connect rc=%d errno=%d", rc, errno);
    }
    lwip_close(sock);
    return ok;
}

static bool nc_mqtt_connected_now(uint32_t gen, bool *fatal_out, bool *client_null_out)
{
    bool connected;
    bool fatal;

    app_mqtt_lock();
    /* 任务书 3.5 会话归属:仅凭全局 connected 不够 —— CONNECTED 事件在
     * mqtt 层打上当时的链路代次(s_mqtt.connected_gen),旧链路的会话
     * (含 stop 窗口内迟到的 CONNACK)不得被新链路复用。 */
    connected = (s_mqtt.client != NULL && s_mqtt.connected &&
                 s_mqtt.connected_gen == gen);
    fatal = s_mqtt.fatal_error;
    if (client_null_out != NULL) {
        *client_null_out = (s_mqtt.client == NULL);
    }
    app_mqtt_unlock();
    if (fatal_out != NULL) {
        *fatal_out = fatal;
    }
    return connected;
}

/* 一次完整探测:事实 → 判决。收尾阶段名写 stage_out(恒非空)。 */
static app_netcheck_state_t nc_run_once(uint32_t gen, char *stage_out,
                                        size_t stage_size)
{
    app_netcheck_facts_t facts = {0};
    char host[64];
    uint16_t port = 0;
    int64_t deadline;
    app_netcheck_verdict_t verdict;
    const char *stage;

    app_config_lock();
    snprintf(host, sizeof(host), "%s", s_config.mqtt_host);
    port = s_config.mqtt_port;
    app_config_unlock();
    facts.mqtt_configured = (host[0] != '\0' && port != 0);

    /* 未配置:无可探目标,判 OK 但阶段记 skipped(不伪装成已验证)。
     * 已配置:必须把 dns_pending 置位再进 judge —— 否则"dns 尚未开始"
     * 会被判成"dns 已失败",秒出 FAIL_INTERNET(2026-11-07 真机抓到的
     * 好网误杀循环即此,2-4s 一轮断连)。 */
    facts.dns_pending = facts.mqtt_configured;
    verdict = app_netcheck_judge(&facts);
    if (verdict != APP_NETCHECK_VERDICT_CONTINUE) {
        stage = app_netcheck_verdict_stage(verdict, &facts);
        snprintf(stage_out, stage_size, "%s", stage);
        return verdict == APP_NETCHECK_VERDICT_OK ? APP_NETCHECK_OK
                                                  : APP_NETCHECK_FAIL_INTERNET;
    }

    deadline = nc_now_ms() + APP_NETCHECK_BUDGET_MS;

    /* 阶段 1:互联网 —— 异步 DNS(评审 P1-1:getaddrinfo 是阻塞调用,
     * 取消/预算都管不住它;dns_gethostbyname 回调式,轮询即可取消)。 */
    {
        err_t dns_err;

        portENTER_CRITICAL(&s_dns.mux);
        s_dns.done = false;
        s_dns.ok = false;
        portEXIT_CRITICAL(&s_dns.mux);
        int64_t dns_deadline = nc_now_ms() + APP_NETCHECK_DNS_BUDGET_MS;

        if (dns_deadline > deadline) {
            dns_deadline = deadline;   /* 不越过任务总预算 */
        }
        dns_err = dns_gethostbyname(host, &s_dns.addr, nc_dns_cb, NULL);
        ESP_LOGI(TAG, "dns host='%s' err=%d ok_pending=%d", host,
                 (int)dns_err, dns_err == ERR_INPROGRESS);
        if (dns_err == ERR_OK) {
            facts.dns_ok = true;   /* IP 字面量,同步返回 */
        } else if (dns_err == ERR_INPROGRESS) {
            while (!s_dns.done && !nc_cancelled_or_superseded(gen) &&
                   nc_now_ms() < dns_deadline) {
                vTaskDelay(pdMS_TO_TICKS(APP_NETCHECK_POLL_MS));
            }
            portENTER_CRITICAL(&s_dns.mux);
            facts.dns_ok = s_dns.done && s_dns.ok;
            portEXIT_CRITICAL(&s_dns.mux);
            if (!facts.dns_ok) {
                ESP_LOGI(TAG, "dns not resolved within %dms budget",
                         APP_NETCHECK_DNS_BUDGET_MS);
            }
        } else {
            facts.dns_ok = false;
        }
        /* DNS 阶段收尾:pending 必须清 —— IP 字面量走 ERR_OK 同步分支时不经
         * INPROGRESS 分支,若不清,judge 会永远 CONTINUE(2026-11-07 真机:
         * conn=1 仍 25s 预算耗尽保守判 fail_mqtt 即此)。 */
        facts.dns_pending = false;
        if (nc_cancelled_or_superseded(gen)) {
            snprintf(stage_out, stage_size, "none");
            return APP_NETCHECK_CANCELLED;
        }
    }

    /* 阶段 1b:TCP 到 broker(≤2 次,单次 2.5s)。 */
    if (facts.dns_ok) {
        for (int i = 0; i < APP_NETCHECK_TCP_ATTEMPTS; i++) {
            int64_t left;

            if (nc_cancelled_or_superseded(gen)) {
                snprintf(stage_out, stage_size, "none");
                return APP_NETCHECK_CANCELLED;
            }
            left = deadline - nc_now_ms();
            if (left <= 0) {
                break;
            }
            facts.tcp_ok = nc_tcp_probe(&s_dns.addr, port,
                                        (uint32_t)(left < APP_NETCHECK_TCP_TIMEOUT_MS
                                                       ? (uint32_t)left
                                                       : APP_NETCHECK_TCP_TIMEOUT_MS));
            if (facts.tcp_ok) {
                break;
            }
            if (i + 1 < APP_NETCHECK_TCP_ATTEMPTS) {
                vTaskDelay(pdMS_TO_TICKS(APP_NETCHECK_TCP_RETRY_GAP_MS));
            }
        }
        facts.tcp_done = true;
    }

    verdict = app_netcheck_judge(&facts);
    if (nc_cancelled_or_superseded(gen)) {
        snprintf(stage_out, stage_size, "none");
        return APP_NETCHECK_CANCELLED;
    }
    if (verdict == APP_NETCHECK_VERDICT_FAIL_INTERNET) {
        stage = app_netcheck_verdict_stage(verdict, &facts);
        snprintf(stage_out, stage_size, "%s", stage);
        return APP_NETCHECK_FAIL_INTERNET;
    }

    /* 阶段 2:MQTT(P2-3 修订版)。先给 4s 宽限:正常 boot/换网路径 mqtt.c
     * 自己的 GOT_IP 处理器已在拉起(+2s backoff),netcheck 若立刻再
     * schedule 会排队出第二次 apply(backoff 5s 一档),把刚连上的链路
     * 无谓拆一次(2026-11-07 真机双重启即此)。宽限内仍不在线,才
     * poke(退避归零+schedule)—— 覆盖 P2-3 的"高退避档 20/30s 卡住"
     * 场景:光 schedule 的 pending 仍要等完整档,必须归零。 */
    verdict = APP_NETCHECK_VERDICT_CONTINUE;
    {
        int64_t grace_deadline = nc_now_ms() + APP_NETCHECK_MQTT_GRACE_MS;

        while (verdict == APP_NETCHECK_VERDICT_CONTINUE &&
               nc_now_ms() < grace_deadline && nc_now_ms() < deadline) {
            if (nc_cancelled_or_superseded(gen)) {
                snprintf(stage_out, stage_size, "none");
                return APP_NETCHECK_CANCELLED;
            }
            facts.mqtt_connected = nc_mqtt_connected_now(gen, &facts.mqtt_fatal, NULL);
            verdict = app_netcheck_judge(&facts);
            if (verdict != APP_NETCHECK_VERDICT_CONTINUE) {
                break;
            }
            vTaskDelay(pdMS_TO_TICKS(APP_NETCHECK_POLL_MS));
        }
        if (verdict == APP_NETCHECK_VERDICT_CONTINUE && !facts.mqtt_connected) {
            ESP_LOGI(TAG, "mqtt not up after grace -> poke restart");
            app_mqtt_poke_restart();
        }
    }
    {
        int polls = 0;

        while (nc_now_ms() < deadline) {
            if (nc_cancelled_or_superseded(gen)) {
                snprintf(stage_out, stage_size, "none");
                return APP_NETCHECK_CANCELLED;
            }
            facts.mqtt_connected = nc_mqtt_connected_now(gen, &facts.mqtt_fatal, NULL);
            verdict = app_netcheck_judge(&facts);
            if (verdict != APP_NETCHECK_VERDICT_CONTINUE) {
                break;
            }
            if ((polls++ % 20) == 0) {
                ESP_LOGI(TAG, "poll n=%d conn=%d fatal=%d", polls,
                         (int)facts.mqtt_connected, (int)facts.mqtt_fatal);
            }
            vTaskDelay(pdMS_TO_TICKS(APP_NETCHECK_POLL_MS));
        }
        ESP_LOGI(TAG, "mqtt wait out: verdict=%d polls=%d conn=%d fatal=%d",
                 (int)verdict, polls, (int)facts.mqtt_connected, (int)facts.mqtt_fatal);
    }
    if (verdict == APP_NETCHECK_VERDICT_CONTINUE) {
        /* 预算耗尽仍无结论:按事实保守收尾(TCP 通 → mqtt 不行;否则 internet)。
         * 绝不把"没验完"当成"通过"。 */
        verdict = facts.tcp_ok ? APP_NETCHECK_VERDICT_FAIL_MQTT
                               : APP_NETCHECK_VERDICT_FAIL_INTERNET;
    }

    stage = app_netcheck_verdict_stage(verdict, &facts);
    snprintf(stage_out, stage_size, "%s", stage);
    if (verdict == APP_NETCHECK_VERDICT_OK) {
        return APP_NETCHECK_OK;
    }
    return verdict == APP_NETCHECK_VERDICT_FAIL_MQTT ? APP_NETCHECK_FAIL_MQTT
                                                     : APP_NETCHECK_FAIL_INTERNET;
}

/* ---- 任务 -------------------------------------------------------------- */

static void nc_task(void *arg)
{
    (void)arg;
    for (;;) {
        uint32_t gen;
        char stage[12] = "none";
        app_netcheck_state_t final_state;
        bool superseded;
        uint32_t next_gen;

        portENTER_CRITICAL(&s_nc.mux);
        gen = s_nc.run_gen;
        portEXIT_CRITICAL(&s_nc.mux);

        final_state = nc_run_once(gen, stage, sizeof(stage));

        portENTER_CRITICAL(&s_nc.mux);
        superseded = (s_nc.pending_gen != 0 && s_nc.pending_gen != gen);
        if (!superseded) {
            s_nc.state = final_state;
            snprintf(s_nc.last_stage, sizeof(s_nc.last_stage), "%s", stage);
        }
        /* 选 B 条件 4:终局快照按代次归档(含被取代/取消的收尾——只要出了
         * 结论就归档;被取代且未出结论不归档,worker 读不到即按超时处理) */
        if (!superseded || final_state != APP_NETCHECK_RUNNING) {
            s_nc.done_valid = true;
            s_nc.done_gen = gen;
            s_nc.done_outcome =
                final_state == APP_NETCHECK_OK ? APP_NETCHECK_OUT_OK :
                final_state == APP_NETCHECK_FAIL_INTERNET ? APP_NETCHECK_OUT_FAIL_INTERNET :
                final_state == APP_NETCHECK_FAIL_MQTT ? APP_NETCHECK_OUT_FAIL_MQTT :
                final_state == APP_NETCHECK_CANCELLED ? APP_NETCHECK_OUT_CANCELLED :
                                                         APP_NETCHECK_OUT_TIMEOUT;
            snprintf(s_nc.done_stage, sizeof(s_nc.done_stage), "%s", stage);
            s_nc.done_ms = nc_now_ms();
        }
        next_gen = s_nc.pending_gen;
        s_nc.pending_gen = 0;
        portEXIT_CRITICAL(&s_nc.mux);

        if (!superseded && final_state != APP_NETCHECK_CANCELLED) {
            ESP_LOGI(TAG, "%s gen=%u stage=%s", nc_state_name(final_state), gen, stage);
            ESP_LOGI(TAG, "stack high watermark=%u bytes",
                     (unsigned)uxTaskGetStackHighWaterMark(NULL) * sizeof(StackType_t));
            if (s_observer != NULL) {
                s_observer(gen, final_state == APP_NETCHECK_OK, stage);
            }
        } else {
            ESP_LOGD(TAG, "gen=%u superseded=%d state=%s", gen, (int)superseded,
                     nc_state_name(final_state));
        }

        portENTER_CRITICAL(&s_nc.mux);
        if (next_gen != 0) {
            /* 排队的代次:原地接续,不重新建任务。 */
            s_nc.run_gen = next_gen;
            s_nc.cancel_req = false;
            s_nc.state = APP_NETCHECK_RUNNING;
            portEXIT_CRITICAL(&s_nc.mux);
            continue;
        }
        if (s_nc.state == APP_NETCHECK_RUNNING) {
            /* 收尾与 start() 竞态:start 已写入新代次但看到 task 非空没建新任务
             * —— 由本任务接续,否则新代次会永远 RUNNING 无人跑。 */
            s_nc.cancel_req = false;
            portEXIT_CRITICAL(&s_nc.mux);
            continue;
        }
        s_nc.task = NULL;
        portEXIT_CRITICAL(&s_nc.mux);
        break;
    }
    vTaskDelete(NULL);
}

/* ---- 公开 API ---------------------------------------------------------- */

esp_err_t app_netcheck_start(uint32_t link_gen)
{
    bool need_task = false;
    bool already_running = false;

    portENTER_CRITICAL(&s_nc.mux);
    if (s_nc.state == APP_NETCHECK_RUNNING) {
        if (s_nc.run_gen == link_gen) {
            already_running = true;   /* 同代次已在验:幂等 */
        } else {
            s_nc.pending_gen = link_gen;   /* 旧代次收尾后自动接续 */
        }
    } else {
        s_nc.state = APP_NETCHECK_RUNNING;
        s_nc.run_gen = link_gen;
        s_nc.pending_gen = 0;
        s_nc.cancel_req = false;
        snprintf(s_nc.last_stage, sizeof(s_nc.last_stage), "none");
        need_task = (s_nc.task == NULL);
    }
    portEXIT_CRITICAL(&s_nc.mux);

    if (already_running || !need_task) {
        return ESP_OK;
    }
    {
        TaskHandle_t handle = NULL;

        if (xTaskCreate(nc_task, "netcheck", 4096, NULL, 4, &handle) != pdPASS) {
            portENTER_CRITICAL(&s_nc.mux);
            s_nc.state = APP_NETCHECK_IDLE;
            portEXIT_CRITICAL(&s_nc.mux);
            ESP_LOGE(TAG, "task create failed");
            return ESP_ERR_NO_MEM;
        }
        portENTER_CRITICAL(&s_nc.mux);
        s_nc.task = handle;
        portEXIT_CRITICAL(&s_nc.mux);
    }
    return ESP_OK;
}

bool app_netcheck_wait(uint32_t link_gen, uint32_t timeout_ms,
                       app_netcheck_result_t *result_out)
{
    int64_t deadline = nc_now_ms() + (int64_t)timeout_ms;
    app_netcheck_state_t st;
    uint32_t rg;

    while (nc_now_ms() < deadline) {
        portENTER_CRITICAL(&s_nc.mux);
        st = s_nc.state;
        rg = s_nc.run_gen;
        portEXIT_CRITICAL(&s_nc.mux);
        if (st != APP_NETCHECK_RUNNING) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_NETCHECK_POLL_MS));
    }
    /* 出口重读一次:deadline 与收尾竞争时以最新终态为准 */
    portENTER_CRITICAL(&s_nc.mux);
    st = s_nc.state;
    rg = s_nc.run_gen;
    portEXIT_CRITICAL(&s_nc.mux);

    if (result_out != NULL) {
        result_out->outcome = app_netcheck_classify(st, rg, link_gen,
                                                    st == APP_NETCHECK_RUNNING);
        result_out->gen = rg;
        if (result_out->outcome == APP_NETCHECK_OUT_TIMEOUT ||
            result_out->outcome == APP_NETCHECK_OUT_GEN_MISMATCH) {
            /* 该两态无"本代次阶段"可言,固定 none,不透传他代次信息 */
            snprintf(result_out->stage, sizeof(result_out->stage), "none");
        } else {
            app_netcheck_last_stage(result_out->stage, sizeof(result_out->stage));
        }
    }
    return result_out != NULL &&
           result_out->outcome == APP_NETCHECK_OUT_OK;
}

void app_netcheck_cancel(uint32_t link_gen)
{
    portENTER_CRITICAL(&s_nc.mux);
    /* 仅取消该代次(任务书 3.4):旧事务清理传入其验证代次,
     * 换代后的新一轮不受影响 */
    if (s_nc.state == APP_NETCHECK_RUNNING && s_nc.run_gen == link_gen) {
        s_nc.cancel_req = true;
    }
    portEXIT_CRITICAL(&s_nc.mux);
}

bool app_netcheck_running_for_gen(uint32_t gen)
{
    bool run;

    portENTER_CRITICAL(&s_nc.mux);
    run = (s_nc.state == APP_NETCHECK_RUNNING && s_nc.run_gen == gen);
    portEXIT_CRITICAL(&s_nc.mux);
    return run;
}

bool app_netcheck_read_done(uint32_t gen, app_netcheck_result_t *out)
{
    bool have;

    portENTER_CRITICAL(&s_nc.mux);
    have = (s_nc.done_valid && s_nc.done_gen == gen);
    if (have && out != NULL) {
        out->outcome = s_nc.done_outcome;
        out->gen = s_nc.done_gen;
        snprintf(out->stage, sizeof(out->stage), "%s", s_nc.done_stage);
    }
    portEXIT_CRITICAL(&s_nc.mux);
    return have;
}

bool app_netcheck_ok_for_gen(uint32_t gen)
{
    bool ok;

    portENTER_CRITICAL(&s_nc.mux);
    ok = (s_nc.state == APP_NETCHECK_OK && s_nc.run_gen == gen);
    portEXIT_CRITICAL(&s_nc.mux);
    return ok;
}

app_netcheck_state_t app_netcheck_state(void)
{
    app_netcheck_state_t st;

    portENTER_CRITICAL(&s_nc.mux);
    st = s_nc.state;
    portEXIT_CRITICAL(&s_nc.mux);
    return st;
}

void app_netcheck_last_stage(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    portENTER_CRITICAL(&s_nc.mux);
    snprintf(out, out_size, "%s", s_nc.last_stage);
    portEXIT_CRITICAL(&s_nc.mux);
}

const char *app_netcheck_outcome_stage(app_netcheck_outcome_t o)
{
    switch (o) {
    case APP_NETCHECK_OUT_FAIL_INTERNET: return "internet";
    case APP_NETCHECK_OUT_FAIL_MQTT: return "mqtt";
    case APP_NETCHECK_OUT_CANCELLED:
    case APP_NETCHECK_OUT_TIMEOUT:
    case APP_NETCHECK_OUT_GEN_MISMATCH:
    default: return "none";
    }
}

void app_netcheck_set_observer(void (*fn)(uint32_t gen, bool ok, const char *stage))
{
    s_observer = fn;
}
