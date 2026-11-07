#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * DEF-NET-001 联网验证 —— 纯策略层(主机可单测)。
 *
 * 两个纯函数:
 *   app_netcheck_judge     事实 → 判决(阶段内判定)
 *   app_netcheck_classify  收尾状态+代次 → 结果(任务书 3.1/3.2:结果结构体,
 *                          不把 CANCELLED/TIMEOUT/GEN_MISMATCH 折叠成 bool)
 *
 * 与任务/套接字/MQTT 完全解耦;netcheck.c 只负责收集事实与驱动时钟。
 *
 * 口径(2026-11-06/07 评审定稿):
 *   - 验证目标 = 当前 MQTT Broker 的网络路径 + 当前链路对应的 MQTT 会话,
 *     不做通用互联网检测;DNS 是域名 Broker 建立 TCP 的前置步骤,
 *     不是独立业务验收阶段;
 *   - DNS 失败/TCP 失败统一 INTERNET_UNVERIFIED;TCP 通而会话未建立统一
 *     MQTT_UNAVAILABLE;不新增 DNS 专用错误码;
 *   - MQTT 未配置不伪装成已验证:判 OK 但阶段记 "skipped";
 *   - 旧代次(会话/结果)一律不得冒充当前代次。
 * ========================================================================== */

/* 收尾状态(netcheck.c 的任务生命周期状态;主机测试只读其取值) */
typedef enum {
    APP_NETCHECK_IDLE = 0,
    APP_NETCHECK_RUNNING,
    APP_NETCHECK_OK,
    APP_NETCHECK_FAIL_INTERNET,
    APP_NETCHECK_FAIL_MQTT,
    APP_NETCHECK_CANCELLED,
} app_netcheck_state_t;

typedef struct {
    bool mqtt_configured; /* mqtt_host 非空且 mqtt_port != 0 */
    bool dns_pending;     /* 异步解析仍在进行(回调未回) */
    bool dns_ok;          /* 解析成功(IP 字面量同步置位) */
    bool tcp_done;        /* TCP connect 已出结果 */
    bool tcp_ok;
    bool mqtt_connected;  /* 当前链路的 MQTT 会话已 CONNECTED(含代次归属) */
    bool mqtt_fatal;      /* 认证被拒等致命错误(自动重连已停) */
} app_netcheck_facts_t;

typedef enum {
    APP_NETCHECK_VERDICT_CONTINUE = 0, /* 事实未齐,继续等 */
    APP_NETCHECK_VERDICT_OK,
    APP_NETCHECK_VERDICT_FAIL_INTERNET,
    APP_NETCHECK_VERDICT_FAIL_MQTT,
} app_netcheck_verdict_t;

typedef enum {
    APP_NETCHECK_OUT_OK = 0,
    APP_NETCHECK_OUT_FAIL_INTERNET,
    APP_NETCHECK_OUT_FAIL_MQTT,
    APP_NETCHECK_OUT_CANCELLED,
    APP_NETCHECK_OUT_TIMEOUT,      /* 预算耗尽仍未出结论(任务书 3.1) */
    APP_NETCHECK_OUT_GEN_MISMATCH, /* 结果属于其他代次(任务书 3.4) */
} app_netcheck_outcome_t;

/* 判决:以当前事实回答"该通过/该失败/继续等"。判定只看事实不看时钟;
 * 时钟归 netcheck.c 的预算管,预算耗尽按"保守失败"收尾。 */
app_netcheck_verdict_t app_netcheck_judge(const app_netcheck_facts_t *facts);

/* 判决对应的阶段名(network.status.net_verify_stage 同一词汇表):
 *   OK + 未配置 → "skipped";OK → "mqtt";FAIL_INTERNET → "internet";
 *   FAIL_MQTT → "mqtt";CONTINUE → ""。
 * 语义注记:internet 在失败侧表达"Broker 路径验证失败"(兼容字段名,
 * 业务含义是 Broker 路径,非通用互联网 —— 任务书 §7.1)。 */
const char *app_netcheck_verdict_stage(app_netcheck_verdict_t v,
                                       const app_netcheck_facts_t *facts);

/* 结果分类:代次不匹配优先于一切(旧代次结果对新链路无意义,任务书 3.4);
 * 终态按状态映射;仍在跑/无任务而等待结束 = TIMEOUT。 */
app_netcheck_outcome_t app_netcheck_classify(app_netcheck_state_t state,
                                             uint32_t run_gen, uint32_t want_gen,
                                             bool timed_out);

/* ============================================================================
 * 选 B(2026-11-07 裁决):tryconnect 成功尾部的"有界推迟拆链"决策。
 *
 * 五条硬边界(裁决附录,见 R-NET 报告 §7):
 *   1. 只挂"本 op 有 netcheck 在飞"——无验证请求(不在飞也无快照)立即拆链,
 *      纯可连探测时序逐字节不变;
 *   2. 预算到期 = EXPIRED(终态 FAILED(verify_timeout)),不是成功、不是无限等;
 *   3. cancel 立即打断(推迟是成功路径的善意等待,不可劣化 cancel 响应);
 *   4. 判决经不可变按代次快照交付(见 netcheck.h app_netcheck_read_done),
 *      worker 只读自己代次的快照,不读 live 状态;
 *   5. 三时序 host 用例:判决出炉→拆链;判决中 cancel→立即拆链;到期→EXPIRED。
 * ========================================================================== */
typedef enum {
    APP_NETCHECK_DEFER_WAIT = 0,     /* 继续等判决 */
    APP_NETCHECK_DEFER_TEARDOWN,     /* 立即拆链(判决已归档或无需等待) */
    APP_NETCHECK_DEFER_EXPIRED,      /* 预算到期:拆链并按 verify_timeout 收尾 */
} app_netcheck_defer_action_t;

app_netcheck_defer_action_t app_netcheck_defer_step(bool in_flight, bool done_ready,
                                                    bool cancelled,
                                                    int64_t waited_ms,
                                                    int64_t budget_ms);
