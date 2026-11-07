#include <stdio.h>
#include <string.h>

#include "netcheck_policy.h"

static int g_fail;
#define CHECK(cond) do { if (!(cond)) { g_fail++; printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); } } while (0)

int main(void)
{
    /* 1. MQTT 未配置:OK + skipped(不伪装成已验证) */
    {
        app_netcheck_facts_t f = { .mqtt_configured = false };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_OK);
        CHECK(strcmp(app_netcheck_verdict_stage(APP_NETCHECK_VERDICT_OK, &f), "skipped") == 0);
    }
    /* 2. DNS 仍在解析:CONTINUE */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_pending = true };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_CONTINUE);
    }
    /* 3. DNS 失败:FAIL_INTERNET(出不了局域网) */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = false, .tcp_done = true };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_FAIL_INTERNET);
        CHECK(strcmp(app_netcheck_verdict_stage(APP_NETCHECK_VERDICT_FAIL_INTERNET, &f), "internet") == 0);
    }
    /* 4. DNS 通但 TCP 未出结果:CONTINUE;TCP 失败:FAIL_INTERNET */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = true };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_CONTINUE);
    }
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = true,
                                   .tcp_done = true, .tcp_ok = false };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_FAIL_INTERNET);
    }
    /* 5. TCP 通,MQTT 未连、非致命:CONTINUE(等 MQTT 自身重连) */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = true,
                                   .tcp_done = true, .tcp_ok = true,
                                   .mqtt_connected = false, .mqtt_fatal = false };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_CONTINUE);
    }
    /* 6. TCP 通,MQTT 致命错误(认证被拒):FAIL_MQTT 立刻判 */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = true,
                                   .tcp_done = true, .tcp_ok = true,
                                   .mqtt_connected = false, .mqtt_fatal = true };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_FAIL_MQTT);
        CHECK(strcmp(app_netcheck_verdict_stage(APP_NETCHECK_VERDICT_FAIL_MQTT, &f), "mqtt") == 0);
    }
    /* 7. 全通:OK,阶段 mqtt */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = true,
                                   .tcp_done = true, .tcp_ok = true,
                                   .mqtt_connected = true, .mqtt_fatal = false };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_OK);
        CHECK(strcmp(app_netcheck_verdict_stage(APP_NETCHECK_VERDICT_OK, &f), "mqtt") == 0);
    }
    /* 8. 两阶段不互混:MQTT 在线不能替 internet 出证据 */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_ok = false,
                                   .tcp_done = false, .tcp_ok = false,
                                   .mqtt_connected = true };
        CHECK(app_netcheck_judge(&f) == APP_NETCHECK_VERDICT_FAIL_INTERNET);
    }
    /* 9. 空指针:保守失败 */
    {
        CHECK(app_netcheck_judge(NULL) == APP_NETCHECK_VERDICT_FAIL_INTERNET);
    }
    /* 10. CONTINUE 阶段名为空串 */
    {
        app_netcheck_facts_t f = { .mqtt_configured = true, .dns_pending = true };
        CHECK(strcmp(app_netcheck_verdict_stage(APP_NETCHECK_VERDICT_CONTINUE, &f), "") == 0);
    }

    /* ===== 结果分类(任务书 3.1/3.2/3.4):不折叠 CANCELLED/TIMEOUT/GEN_MISMATCH ===== */
    /* 11. 当前代次终态逐一映射 */
    {
        CHECK(app_netcheck_classify(APP_NETCHECK_OK, 5, 5, false) == APP_NETCHECK_OUT_OK);
        CHECK(app_netcheck_classify(APP_NETCHECK_FAIL_INTERNET, 5, 5, false) == APP_NETCHECK_OUT_FAIL_INTERNET);
        CHECK(app_netcheck_classify(APP_NETCHECK_FAIL_MQTT, 5, 5, false) == APP_NETCHECK_OUT_FAIL_MQTT);
        CHECK(app_netcheck_classify(APP_NETCHECK_CANCELLED, 5, 5, false) == APP_NETCHECK_OUT_CANCELLED);
    }
    /* 12. 代次不匹配优先于状态:旧代次的 OK/FAIL 对新链路一律 GEN_MISMATCH */
    {
        CHECK(app_netcheck_classify(APP_NETCHECK_OK, 4, 5, false) == APP_NETCHECK_OUT_GEN_MISMATCH);
        CHECK(app_netcheck_classify(APP_NETCHECK_FAIL_MQTT, 4, 5, false) == APP_NETCHECK_OUT_GEN_MISMATCH);
        CHECK(app_netcheck_classify(APP_NETCHECK_RUNNING, 4, 5, true) == APP_NETCHECK_OUT_GEN_MISMATCH);
    }
    /* 13. 等待超时:仍在跑/无任务 → TIMEOUT(不伪装成失败或通过) */
    {
        CHECK(app_netcheck_classify(APP_NETCHECK_RUNNING, 5, 5, true) == APP_NETCHECK_OUT_TIMEOUT);
        CHECK(app_netcheck_classify(APP_NETCHECK_IDLE, 5, 5, true) == APP_NETCHECK_OUT_TIMEOUT);
    }
    /* 14. 同代次在跑但未超时(等待被终态竞争,重读后已终态)按状态走 */
    {
        CHECK(app_netcheck_classify(APP_NETCHECK_RUNNING, 5, 5, false) == APP_NETCHECK_OUT_TIMEOUT);
    }

    /* ===== 选 B:推迟拆链决策(裁决五条件,三时序) ===== */
    /* 时序 1:判决出炉 → 立即拆链(worker 读快照通过) */
    {
        CHECK(app_netcheck_defer_step(true, true, false, 3000, 25000) == APP_NETCHECK_DEFER_TEARDOWN);
    }
    /* 时序 2:判决中 cancel → 立即拆链(优先级最高,不劣化 cancel 响应) */
    {
        CHECK(app_netcheck_defer_step(true, false, true, 1000, 25000) == APP_NETCHECK_DEFER_TEARDOWN);
        CHECK(app_netcheck_defer_step(true, true, true, 1000, 25000) == APP_NETCHECK_DEFER_TEARDOWN);
    }
    /* 时序 3:25s 到期 → EXPIRED(verify_timeout),不是成功、不是无限等 */
    {
        CHECK(app_netcheck_defer_step(true, false, false, 25000, 25000) == APP_NETCHECK_DEFER_EXPIRED);
        CHECK(app_netcheck_defer_step(true, false, false, 26000, 25000) == APP_NETCHECK_DEFER_EXPIRED);
    }
    /* 条件 1:无验证在飞且无快照 → 立即拆链(纯可连探测时序不变) */
    {
        CHECK(app_netcheck_defer_step(false, false, false, 0, 25000) == APP_NETCHECK_DEFER_TEARDOWN);
    }
    /* 正常等待中 */
    {
        CHECK(app_netcheck_defer_step(true, false, false, 1000, 25000) == APP_NETCHECK_DEFER_WAIT);
        CHECK(app_netcheck_defer_step(true, false, false, 24999, 25000) == APP_NETCHECK_DEFER_WAIT);
    }

    printf("netcheck_policy: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
