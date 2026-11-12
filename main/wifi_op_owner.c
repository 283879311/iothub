/* WIFI-OP-RESOURCE-RACE 纯状态机实现（主机可测，不依赖 ESP-IDF/FreeRTOS）。 */

#include "wifi_op_owner.h"

app_wifi_owner_state_t app_wifi_owner_init(void)
{
    app_wifi_owner_state_t st = {
        .owner = APP_WIFI_OWNER_NONE,
        .tryx_waiting = false,
        .cleanup_requested = false,
    };
    return st;
}

static app_wifi_owner_step_t make(const app_wifi_owner_state_t *next,
                                  app_wifi_owner_act_t act)
{
    app_wifi_owner_step_t out = { .state = *next, .action = act };
    return out;
}

app_wifi_owner_step_t app_wifi_owner_step(const app_wifi_owner_state_t *in,
                                          app_wifi_owner_ev_t ev)
{
    app_wifi_owner_state_t st = *in;
    app_wifi_owner_act_t act = APP_WIFI_OWNER_ACT_NONE;

    switch (ev) {
    case APP_WIFI_OWNER_EV_CLAIM_TRYX:
        /* 等待者位由调用方在首轮之前置位（与 CAS 同一临界区）；
         * 这里只负责接受/等待/失败判定。 */
        if (st.owner == APP_WIFI_OWNER_CLEANUP_FAILED) {
            act = APP_WIFI_OWNER_ACT_DENY_FAILED;
            st.tryx_waiting = false;
        } else if (st.owner == APP_WIFI_OWNER_NONE) {
            st.owner = APP_WIFI_OWNER_TRYX;
            st.tryx_waiting = false;
            act = APP_WIFI_OWNER_ACT_ACCEPT;
        } else {
            act = APP_WIFI_OWNER_ACT_WAIT;
        }
        break;

    case APP_WIFI_OWNER_EV_TRYX_GIVEUP:
        st.tryx_waiting = false;
        act = APP_WIFI_OWNER_ACT_NONE;
        break;

    case APP_WIFI_OWNER_EV_CLAIM_AUTO:
        if (st.owner == APP_WIFI_OWNER_CLEANUP_FAILED) {
            act = APP_WIFI_OWNER_ACT_DENY_FAILED;
        } else if (st.owner != APP_WIFI_OWNER_NONE) {
            /* owner 忙是首要阻塞原因（日志更准确）；waiter 判定只在 owner 空闲时生效 */
            act = APP_WIFI_OWNER_ACT_DENY_BUSY;
        } else if (st.tryx_waiting) {
            /* C1：等待者优先——互斥之外补进展性 */
            act = APP_WIFI_OWNER_ACT_DENY_WAITER;
        } else {
            st.owner = APP_WIFI_OWNER_AUTO;
            act = APP_WIFI_OWNER_ACT_ACCEPT;
        }
        break;

    case APP_WIFI_OWNER_EV_CLAIM_APPLY:
        if (st.owner == APP_WIFI_OWNER_CLEANUP_FAILED) {
            act = APP_WIFI_OWNER_ACT_DENY_FAILED;
        } else if (st.owner == APP_WIFI_OWNER_NONE) {
            st.owner = APP_WIFI_OWNER_APPLY;
            act = APP_WIFI_OWNER_ACT_ACCEPT;
        } else {
            act = APP_WIFI_OWNER_ACT_WAIT;
        }
        break;

    case APP_WIFI_OWNER_EV_RELEASE_TRYX:
    case APP_WIFI_OWNER_EV_RELEASE_AUTO:
    case APP_WIFI_OWNER_EV_RELEASE_APPLY: {
        app_wifi_owner_t from =
            (ev == APP_WIFI_OWNER_EV_RELEASE_TRYX) ? APP_WIFI_OWNER_TRYX :
            (ev == APP_WIFI_OWNER_EV_RELEASE_AUTO) ? APP_WIFI_OWNER_AUTO :
                                                     APP_WIFI_OWNER_APPLY;
        if (st.owner != from) {
            /* 配错门/陈旧事件：不改变状态 */
            act = APP_WIFI_OWNER_ACT_NONE;
            break;
        }
        if (st.cleanup_requested) {
            /* B1：释放即交接，不产生可被抢占的 NONE 窗口 */
            st.owner = APP_WIFI_OWNER_CLEANUP;
            st.cleanup_requested = false;
            act = APP_WIFI_OWNER_ACT_HANDOVER;
        } else {
            st.owner = APP_WIFI_OWNER_NONE;
            act = APP_WIFI_OWNER_ACT_NONE;
        }
        break;
    }

    case APP_WIFI_OWNER_EV_CLEANUP_REQUEST:
        if (st.owner == APP_WIFI_OWNER_CLEANUP_FAILED) {
            act = APP_WIFI_OWNER_ACT_DENY_FAILED;
        } else if (st.owner == APP_WIFI_OWNER_CLEANUP) {
            act = APP_WIFI_OWNER_ACT_NOTIFY;   /* 已在清理：重复请求幂等 */
        } else if (st.owner == APP_WIFI_OWNER_NONE) {
            st.owner = APP_WIFI_OWNER_CLEANUP;
            st.cleanup_requested = false;
            act = APP_WIFI_OWNER_ACT_NOTIFY;
        } else {
            /* TRYX/AUTO/APPLY 持有：等其释放时移交 */
            st.cleanup_requested = true;
            act = APP_WIFI_OWNER_ACT_NONE;
        }
        break;

    case APP_WIFI_OWNER_EV_CLEANUP_DONE:
        if (st.owner == APP_WIFI_OWNER_CLEANUP) {
            st.owner = APP_WIFI_OWNER_NONE;
            act = APP_WIFI_OWNER_ACT_NONE;
        }
        break;

    case APP_WIFI_OWNER_EV_CLEANUP_FAIL:
        if (st.owner == APP_WIFI_OWNER_CLEANUP) {
            st.owner = APP_WIFI_OWNER_CLEANUP_FAILED;
            act = APP_WIFI_OWNER_ACT_NONE;
        }
        break;

    case APP_WIFI_OWNER_EV_BOOT_RESET:
        st = app_wifi_owner_init();
        act = APP_WIFI_OWNER_ACT_NONE;
        break;

    default:
        act = APP_WIFI_OWNER_ACT_NONE;
        break;
    }

    return make(&st, act);
}

const char *app_wifi_owner_name(app_wifi_owner_t owner)
{
    switch (owner) {
    case APP_WIFI_OWNER_NONE:           return "NONE";
    case APP_WIFI_OWNER_AUTO:           return "AUTO";
    case APP_WIFI_OWNER_TRYX:           return "TRYX";
    case APP_WIFI_OWNER_APPLY:          return "APPLY";
    case APP_WIFI_OWNER_CLEANUP:        return "CLEANUP";
    case APP_WIFI_OWNER_CLEANUP_FAILED: return "CLEANUP_FAILED";
    default:                            return "?";
    }
}

const char *app_wifi_owner_act_name(app_wifi_owner_act_t act)
{
    switch (act) {
    case APP_WIFI_OWNER_ACT_NONE:        return "none";
    case APP_WIFI_OWNER_ACT_ACCEPT:      return "accept";
    case APP_WIFI_OWNER_ACT_WAIT:        return "wait";
    case APP_WIFI_OWNER_ACT_DENY_BUSY:   return "deny_busy";
    case APP_WIFI_OWNER_ACT_DENY_WAITER: return "deny_waiter";
    case APP_WIFI_OWNER_ACT_DENY_FAILED: return "deny_failed";
    case APP_WIFI_OWNER_ACT_NOTIFY:      return "notify";
    case APP_WIFI_OWNER_ACT_HANDOVER:    return "handover";
    default:                             return "?";
    }
}

const char *app_wifi_owner_ev_name(app_wifi_owner_ev_t ev)
{
    switch (ev) {
    case APP_WIFI_OWNER_EV_CLAIM_TRYX:     return "claim_tryx";
    case APP_WIFI_OWNER_EV_TRYX_GIVEUP:    return "tryx_giveup";
    case APP_WIFI_OWNER_EV_CLAIM_AUTO:     return "claim_auto";
    case APP_WIFI_OWNER_EV_CLAIM_APPLY:    return "claim_apply";
    case APP_WIFI_OWNER_EV_RELEASE_TRYX:   return "release_tryx";
    case APP_WIFI_OWNER_EV_RELEASE_AUTO:   return "release_auto";
    case APP_WIFI_OWNER_EV_RELEASE_APPLY:  return "release_apply";
    case APP_WIFI_OWNER_EV_CLEANUP_REQUEST:return "cleanup_request";
    case APP_WIFI_OWNER_EV_CLEANUP_DONE:   return "cleanup_done";
    case APP_WIFI_OWNER_EV_CLEANUP_FAIL:   return "cleanup_fail";
    case APP_WIFI_OWNER_EV_BOOT_RESET:     return "boot_reset";
    default:                               return "?";
    }
}

bool app_wifi_owner_would_accept(const app_wifi_owner_state_t *st,
                                 app_wifi_owner_ev_t ev)
{
    app_wifi_owner_step_t r = app_wifi_owner_step(st, ev);
    return r.action == APP_WIFI_OWNER_ACT_ACCEPT;
}
