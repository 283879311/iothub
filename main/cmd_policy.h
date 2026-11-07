#pragma once

#include <stdbool.h>

#include "cmd_context.h"
#include "operation.h"

/* Stage 0 command policy: pure source/method/owner decisions.
 * No Wi-Fi/MQTT/cJSON dependencies; host-testable. */

#ifndef APP_CMD_MQTT_NET_TXN_ENABLE
#define APP_CMD_MQTT_NET_TXN_ENABLE 1
#endif

typedef enum {
    APP_CMD_POLICY_ALLOW = 0,
    APP_CMD_POLICY_FORBIDDEN,
    APP_CMD_POLICY_BUSY,
    APP_CMD_POLICY_INVALID_ARGUMENT,
    APP_CMD_POLICY_NOT_ENABLED,
} app_cmd_policy_result_t;

typedef enum {
    APP_CMD_KIND_OTHER = 0,
    APP_CMD_KIND_NET_STATUS,
    APP_CMD_KIND_NET_TRY,
    APP_CMD_KIND_NET_APPLY,
    APP_CMD_KIND_NET_CANCEL,
    APP_CMD_KIND_OPERATION_GET,
    APP_CMD_KIND_NET_PROFILE_DELETE,
    APP_CMD_KIND_NET_SCAN,
    APP_CMD_KIND_NET_RECONNECT,
    APP_CMD_KIND_CONFIG_SET,
} app_cmd_kind_t;

app_cmd_kind_t app_cmd_kind(const char *method);
bool app_cmd_is_network_method(const char *method);

/* External command-plane decision. `.async` 已随 v0 一并删除;internal
 * apply children use `network.apply`, not `.async`. */
app_cmd_policy_result_t app_cmd_policy_check_network(const app_cmd_context_t *ctx,
                                                     const char *method);

/* Apply must be performed by the same owner that created the transaction.
 * INTERNAL children inherit the origin owner. */
app_cmd_policy_result_t app_cmd_policy_check_apply_owner(const app_cmd_context_t *ctx,
                                                         app_operation_owner_t owner);

/* Cancel requires an authenticated caller and a matching operation owner.
 * BLE is the field-rescue channel and may cancel any network owner. */
bool app_cmd_cancel_allowed(const app_cmd_context_t *ctx,
                            app_operation_owner_t owner);

bool app_cmd_mqtt_net_txn_enabled(void);
const char *app_cmd_policy_build_variant(void);
