#include "operation_state.h"

const char *app_op_state_name(app_op_state_t state)
{
    switch (state) {
    case APP_OP_FREE:       return "free";
    case APP_OP_ACCEPTED:   return "accepted";
    case APP_OP_CONNECTING: return "connecting";
    case APP_OP_GOT_IP:     return "got_ip";
    case APP_OP_COMMITTING: return "committing";
    case APP_OP_SUCCESS:    return "success";
    case APP_OP_FAILED:     return "failed";
    case APP_OP_CANCELLED:  return "cancelled";
    case APP_OP_EXPIRED:    return "expired";
    default:                return "unknown";
    }
}

bool app_op_state_is_terminal(app_op_state_t state)
{
    return state == APP_OP_SUCCESS || state == APP_OP_FAILED ||
           state == APP_OP_CANCELLED || state == APP_OP_EXPIRED;
}

bool app_op_state_is_active(app_op_state_t state)
{
    return state == APP_OP_ACCEPTED || state == APP_OP_CONNECTING ||
           state == APP_OP_GOT_IP || state == APP_OP_COMMITTING;
}

bool app_op_state_can_cancel(app_op_state_t state)
{
    return state == APP_OP_ACCEPTED || state == APP_OP_CONNECTING ||
           state == APP_OP_GOT_IP;
}

bool app_op_transition_allowed(app_op_state_t from, app_op_state_t to)
{
    if (from == APP_OP_FREE) {
        return to == APP_OP_ACCEPTED;
    }
    if (app_op_state_is_terminal(from)) {
        return to == APP_OP_FREE;   /* 仅槽位回收 */
    }
    switch (from) {
    case APP_OP_ACCEPTED:
        return to == APP_OP_CONNECTING || to == APP_OP_CANCELLED ||
               to == APP_OP_FAILED || to == APP_OP_EXPIRED;
    case APP_OP_CONNECTING:
        return to == APP_OP_GOT_IP || to == APP_OP_CANCELLED ||
               to == APP_OP_FAILED || to == APP_OP_EXPIRED;
    case APP_OP_GOT_IP:
        return to == APP_OP_COMMITTING || to == APP_OP_CANCELLED ||
               to == APP_OP_FAILED || to == APP_OP_EXPIRED;
    case APP_OP_COMMITTING:
        return to == APP_OP_SUCCESS || to == APP_OP_FAILED;
    default:
        return false;
    }
}
