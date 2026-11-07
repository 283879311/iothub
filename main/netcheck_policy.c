#include <stddef.h>

#include "netcheck_policy.h"

app_netcheck_verdict_t app_netcheck_judge(const app_netcheck_facts_t *facts)
{
    if (facts == NULL) {
        return APP_NETCHECK_VERDICT_FAIL_INTERNET;
    }
    /* 无可探目标(未配置 MQTT):判 OK,阶段记 skipped —— 不伪装成"已验证",
     * 由阶段名明确表达"这项检查被跳过"。 */
    if (!facts->mqtt_configured) {
        return APP_NETCHECK_VERDICT_OK;
    }
    if (facts->dns_pending) {
        return APP_NETCHECK_VERDICT_CONTINUE;
    }
    if (!facts->dns_ok) {
        /* 解析失败/超时:出不了局域网,归 internet */
        return APP_NETCHECK_VERDICT_FAIL_INTERNET;
    }
    if (!facts->tcp_done) {
        return APP_NETCHECK_VERDICT_CONTINUE;
    }
    if (!facts->tcp_ok) {
        return APP_NETCHECK_VERDICT_FAIL_INTERNET;
    }
    if (!facts->mqtt_connected) {
        /* TCP 通但 MQTT 没连上:致命错误(broker 明确拒绝)立刻判,
         * 否则等 —— 由 netcheck.c 的总预算兜底收尾。 */
        return facts->mqtt_fatal ? APP_NETCHECK_VERDICT_FAIL_MQTT
                                 : APP_NETCHECK_VERDICT_CONTINUE;
    }
    return APP_NETCHECK_VERDICT_OK;
}

const char *app_netcheck_verdict_stage(app_netcheck_verdict_t v,
                                       const app_netcheck_facts_t *facts)
{
    switch (v) {
    case APP_NETCHECK_VERDICT_OK:
        return (facts != NULL && !facts->mqtt_configured) ? "skipped" : "mqtt";
    case APP_NETCHECK_VERDICT_FAIL_INTERNET:
        return "internet";
    case APP_NETCHECK_VERDICT_FAIL_MQTT:
        return "mqtt";
    case APP_NETCHECK_VERDICT_CONTINUE:
    default:
        return "";
    }
}

app_netcheck_defer_action_t app_netcheck_defer_step(bool in_flight, bool done_ready,
                                                    bool cancelled,
                                                    int64_t waited_ms,
                                                    int64_t budget_ms)
{
    /* 条件 3:cancel 优先级最高——用户取消/断连必须立即拆链 */
    if (cancelled) {
        return APP_NETCHECK_DEFER_TEARDOWN;
    }
    /* 条件 4:判决已归档(不可变快照)——拆链,worker 读快照 */
    if (done_ready) {
        return APP_NETCHECK_DEFER_TEARDOWN;
    }
    /* 条件 1:无验证在飞也无快照——无验证请求,维持原 0.5s 拆链时序 */
    if (!in_flight) {
        return APP_NETCHECK_DEFER_TEARDOWN;
    }
    /* 条件 2:预算到期即 EXPIRED,不无限等 */
    if (waited_ms >= budget_ms) {
        return APP_NETCHECK_DEFER_EXPIRED;
    }
    return APP_NETCHECK_DEFER_WAIT;
}

app_netcheck_outcome_t app_netcheck_classify(app_netcheck_state_t state,
                                             uint32_t run_gen, uint32_t want_gen,
                                             bool timed_out)
{
    (void)timed_out; /* 语义由调用方保证:RUNNING+等待结束 ⇒ TIMEOUT */
    /* 代次不匹配优先:旧/新代次的结果对当前等待者都无意义(任务书 3.4) */
    if (run_gen != want_gen) {
        return APP_NETCHECK_OUT_GEN_MISMATCH;
    }
    switch (state) {
    case APP_NETCHECK_OK:
        return APP_NETCHECK_OUT_OK;
    case APP_NETCHECK_FAIL_INTERNET:
        return APP_NETCHECK_OUT_FAIL_INTERNET;
    case APP_NETCHECK_FAIL_MQTT:
        return APP_NETCHECK_OUT_FAIL_MQTT;
    case APP_NETCHECK_CANCELLED:
        return APP_NETCHECK_OUT_CANCELLED;
    case APP_NETCHECK_RUNNING:
    case APP_NETCHECK_IDLE:
    default:
        return APP_NETCHECK_OUT_TIMEOUT;
    }
}
