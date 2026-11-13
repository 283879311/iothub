/* WIFI-OP-RESOURCE-RACE 纯状态机主机测试（P1：+12 例从建议改必交）。 */

#include <stdio.h>
#include <string.h>

#include "wifi_op_owner.h"

static int g_pass;
static int g_fail;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                    \
    } while (0)

static app_wifi_owner_state_t st(app_wifi_owner_t owner, bool waiting, bool requested)
{
    app_wifi_owner_state_t s = {
        .owner = owner,
        .tryx_waiting = waiting,
        .cleanup_requested = requested,
    };
    return s;
}

static app_wifi_owner_step_t step(const app_wifi_owner_state_t *s, app_wifi_owner_ev_t ev)
{
    return app_wifi_owner_step(s, ev);
}

/* 1-5：TRYX 取得与互斥 */
static void test_claim_tryx(void)
{
    app_wifi_owner_state_t s = app_wifi_owner_init();
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_CLAIM_TRYX);
    CHECK(r.action == APP_WIFI_OWNER_ACT_ACCEPT);
    CHECK(r.state.owner == APP_WIFI_OWNER_TRYX);
    CHECK(!r.state.tryx_waiting);

    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_AUTO);          /* TRYX 不持有时另测 */
    CHECK(r.action == APP_WIFI_OWNER_ACT_ACCEPT);

    s = st(APP_WIFI_OWNER_TRYX, true, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_TRYX);
    CHECK(r.action == APP_WIFI_OWNER_ACT_WAIT);          /* 单飞 */
    CHECK(r.state.owner == APP_WIFI_OWNER_TRYX);

    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_AUTO);
    CHECK(r.action == APP_WIFI_OWNER_ACT_DENY_BUSY);     /* 互斥 */

    s = st(APP_WIFI_OWNER_CLEANUP, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_TRYX);
    CHECK(r.action == APP_WIFI_OWNER_ACT_WAIT);          /* 等 cleanup */
}

/* 6-8：CLEANUP_FAILED 粘性 */
static void test_cleanup_failed_sticky(void)
{
    app_wifi_owner_state_t s = st(APP_WIFI_OWNER_CLEANUP_FAILED, false, false);
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_CLAIM_TRYX);
    CHECK(r.action == APP_WIFI_OWNER_ACT_DENY_FAILED);
    CHECK(!r.state.tryx_waiting);

    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_AUTO);
    CHECK(r.action == APP_WIFI_OWNER_ACT_DENY_FAILED);

    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_APPLY);
    CHECK(r.action == APP_WIFI_OWNER_ACT_DENY_FAILED);

    r = step(&s, APP_WIFI_OWNER_EV_BOOT_RESET);
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);
    r = step(&r.state, APP_WIFI_OWNER_EV_CLAIM_TRYX);
    CHECK(r.action == APP_WIFI_OWNER_ACT_ACCEPT);
}

/* 9-11：C1 waiter 优先 */
static void test_waiter_priority(void)
{
    app_wifi_owner_state_t s = st(APP_WIFI_OWNER_NONE, true, false);
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_CLAIM_AUTO);
    CHECK(r.action == APP_WIFI_OWNER_ACT_DENY_WAITER);
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);

    r = step(&s, APP_WIFI_OWNER_EV_TRYX_GIVEUP);
    CHECK(!r.state.tryx_waiting);
    r = step(&r.state, APP_WIFI_OWNER_EV_CLAIM_AUTO);
    CHECK(r.action == APP_WIFI_OWNER_ACT_ACCEPT);        /* waiter 清位后恢复 */

    s = st(APP_WIFI_OWNER_TRYX, true, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_AUTO);
    CHECK(r.action == APP_WIFI_OWNER_ACT_DENY_BUSY);     /* owner 忙优先于 waiter 判定 */
}

/* 12-15：cleanup 请求与无窗口移交 */
static void test_cleanup_handover(void)
{
    app_wifi_owner_state_t s = app_wifi_owner_init();
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_REQUEST);
    CHECK(r.action == APP_WIFI_OWNER_ACT_NOTIFY);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP);

    s = st(APP_WIFI_OWNER_TRYX, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_REQUEST);
    CHECK(r.action == APP_WIFI_OWNER_ACT_NONE);
    CHECK(r.state.owner == APP_WIFI_OWNER_TRYX);
    CHECK(r.state.cleanup_requested);

    r = step(&r.state, APP_WIFI_OWNER_EV_RELEASE_TRYX);
    CHECK(r.action == APP_WIFI_OWNER_ACT_HANDOVER);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP);
    CHECK(!r.state.cleanup_requested);

    s = st(APP_WIFI_OWNER_AUTO, false, true);
    r = step(&s, APP_WIFI_OWNER_EV_RELEASE_AUTO);
    CHECK(r.action == APP_WIFI_OWNER_ACT_HANDOVER);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP);

    s = st(APP_WIFI_OWNER_APPLY, false, true);
    r = step(&s, APP_WIFI_OWNER_EV_RELEASE_APPLY);
    CHECK(r.action == APP_WIFI_OWNER_ACT_HANDOVER);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP);

    /* 重复请求幂等 */
    s = st(APP_WIFI_OWNER_CLEANUP, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_REQUEST);
    CHECK(r.action == APP_WIFI_OWNER_ACT_NOTIFY);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP);
}

/* 16-19：cleanup 完成/失败/普通释放/非法释放 */
static void test_release_and_terminal(void)
{
    app_wifi_owner_state_t s = st(APP_WIFI_OWNER_CLEANUP, false, false);
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_DONE);
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);

    r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_FAIL);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP_FAILED);

    s = st(APP_WIFI_OWNER_TRYX, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_RELEASE_TRYX);
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);
    CHECK(r.action == APP_WIFI_OWNER_ACT_NONE);

    s = st(APP_WIFI_OWNER_NONE, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_RELEASE_TRYX);        /* 陈旧/配错门的释放 */
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);
    CHECK(r.action == APP_WIFI_OWNER_ACT_NONE);

    s = st(APP_WIFI_OWNER_AUTO, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLAIM_APPLY);
    CHECK(r.action == APP_WIFI_OWNER_ACT_WAIT);          /* APPLY 阻塞等待 */
}

/* fencing 不变量:CLEANUP 态下除 DONE/FAIL 外,任何事件都不得改变 owner——
 * 即 watchdog(只置 abort 标志,不投递事件)在状态机层面没有夺权路径。 */
static void test_cleanup_fencing_invariants(void)
{
    const app_wifi_owner_ev_t evs[] = {
        APP_WIFI_OWNER_EV_CLAIM_TRYX,
        APP_WIFI_OWNER_EV_CLAIM_AUTO,
        APP_WIFI_OWNER_EV_CLAIM_APPLY,
        APP_WIFI_OWNER_EV_RELEASE_TRYX,
        APP_WIFI_OWNER_EV_RELEASE_AUTO,
        APP_WIFI_OWNER_EV_RELEASE_APPLY,
        APP_WIFI_OWNER_EV_CLEANUP_REQUEST,
    };
    app_wifi_owner_state_t s = st(APP_WIFI_OWNER_CLEANUP, false, false);

    for (unsigned i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
        app_wifi_owner_step_t r = step(&s, evs[i]);
        CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP);
    }
    /* 只有任务自身(安全点)投递的 DONE/FAIL 能改变 owner */
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_DONE);
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);
    s = st(APP_WIFI_OWNER_CLEANUP, false, false);
    r = step(&s, APP_WIFI_OWNER_EV_CLEANUP_FAIL);
    CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP_FAILED);
}

/* fencing 不变量:CLEANUP_FAILED 粘性——除 BOOT_RESET 外全部拒绝或不变 */
static void test_cleanup_failed_invariants(void)
{
    const app_wifi_owner_ev_t evs[] = {
        APP_WIFI_OWNER_EV_CLAIM_TRYX,
        APP_WIFI_OWNER_EV_CLAIM_AUTO,
        APP_WIFI_OWNER_EV_CLAIM_APPLY,
        APP_WIFI_OWNER_EV_RELEASE_TRYX,
        APP_WIFI_OWNER_EV_RELEASE_AUTO,
        APP_WIFI_OWNER_EV_RELEASE_APPLY,
        APP_WIFI_OWNER_EV_CLEANUP_REQUEST,
        APP_WIFI_OWNER_EV_CLEANUP_DONE,
        APP_WIFI_OWNER_EV_CLEANUP_FAIL,
    };
    app_wifi_owner_state_t s = st(APP_WIFI_OWNER_CLEANUP_FAILED, false, false);

    for (unsigned i = 0; i < sizeof(evs) / sizeof(evs[0]); i++) {
        app_wifi_owner_step_t r = step(&s, evs[i]);
        CHECK(r.state.owner == APP_WIFI_OWNER_CLEANUP_FAILED);
    }
    app_wifi_owner_step_t r = step(&s, APP_WIFI_OWNER_EV_BOOT_RESET);
    CHECK(r.state.owner == APP_WIFI_OWNER_NONE);
}

int main(void)
{
    test_claim_tryx();
    test_cleanup_failed_sticky();
    test_waiter_priority();
    test_cleanup_handover();
    test_release_and_terminal();
    test_cleanup_fencing_invariants();
    test_cleanup_failed_invariants();

    printf("wifi_owner: %d passed, %d failed\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
