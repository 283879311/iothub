#pragma once

#include <stdbool.h>

/* G3 操作状态机的纯逻辑：不依赖 ESP-IDF/FreeRTOS，便于主机侧单元测试。 */
typedef enum {
    APP_OP_FREE = 0,
    APP_OP_ACCEPTED,
    APP_OP_CONNECTING,
    APP_OP_GOT_IP,
    APP_OP_COMMITTING,
    APP_OP_SUCCESS,
    APP_OP_FAILED,
    APP_OP_CANCELLED,
    APP_OP_EXPIRED,
    APP_OP_STATE_COUNT,
} app_op_state_t;

const char *app_op_state_name(app_op_state_t state);
bool app_op_state_is_terminal(app_op_state_t state);
bool app_op_state_is_active(app_op_state_t state);
bool app_op_state_can_cancel(app_op_state_t state);
bool app_op_transition_allowed(app_op_state_t from, app_op_state_t to);
