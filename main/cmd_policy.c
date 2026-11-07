#include <string.h>

#include "cmd_policy.h"

static bool app_cmd_source_is_entry(app_cmd_source_t source)
{
    return source == APP_CMD_SOURCE_MQTT || source == APP_CMD_SOURCE_BLE ||
           source == APP_CMD_SOURCE_HTTP || source == APP_CMD_SOURCE_TOUCH ||
           source == APP_CMD_SOURCE_SERIAL;
}

static app_operation_owner_t app_cmd_ctx_owner(const app_cmd_context_t *ctx)
{
    app_cmd_source_t source;

    if (ctx == NULL) {
        return APP_OPERATION_OWNER_INTERNAL;
    }
    source = (ctx->exec_env == APP_CMD_EXEC_INTERNAL) ? ctx->origin : ctx->source;
    switch (source) {
    case APP_CMD_SOURCE_BLE:  return APP_OPERATION_OWNER_BLE;
    case APP_CMD_SOURCE_HTTP: return APP_OPERATION_OWNER_HTTP;
    case APP_CMD_SOURCE_MQTT: return APP_OPERATION_OWNER_MQTT;
    default:                  return APP_OPERATION_OWNER_INTERNAL;
    }
}

app_cmd_kind_t app_cmd_kind(const char *method)
{
    if (method == NULL) {
        return APP_CMD_KIND_OTHER;
    }
    if (strcmp(method, "network.status") == 0) return APP_CMD_KIND_NET_STATUS;
    if (strcmp(method, "network.tryconnect") == 0) return APP_CMD_KIND_NET_TRY;
    if (strcmp(method, "network.apply") == 0) return APP_CMD_KIND_NET_APPLY;
    if (strcmp(method, "network.cancel") == 0) return APP_CMD_KIND_NET_CANCEL;
    if (strcmp(method, "operation.get") == 0) return APP_CMD_KIND_OPERATION_GET;
    if (strcmp(method, "network.profile.delete") == 0) return APP_CMD_KIND_NET_PROFILE_DELETE;
    if (strcmp(method, "network.scan") == 0) return APP_CMD_KIND_NET_SCAN;
    if (strcmp(method, "network.reconnect") == 0) return APP_CMD_KIND_NET_RECONNECT;
    if (strcmp(method, "config.set") == 0) return APP_CMD_KIND_CONFIG_SET;
    return APP_CMD_KIND_OTHER;
}

bool app_cmd_is_network_method(const char *method)
{
    app_cmd_kind_t kind = app_cmd_kind(method);

    return kind >= APP_CMD_KIND_NET_STATUS && kind <= APP_CMD_KIND_NET_RECONNECT;
}

app_cmd_policy_result_t app_cmd_policy_check_network(const app_cmd_context_t *ctx,
                                                     const char *method)
{
    app_cmd_kind_t kind;
    bool internal;
    app_cmd_source_t entry_source;

    if (ctx == NULL || !app_cmd_ctx_valid(ctx)) {
        return APP_CMD_POLICY_FORBIDDEN;
    }
    if (method == NULL) {
        return APP_CMD_POLICY_ALLOW;
    }
    if (!app_cmd_is_network_method(method)) {
        return APP_CMD_POLICY_ALLOW;
    }

    internal = (ctx->exec_env == APP_CMD_EXEC_INTERNAL);
    entry_source = internal ? ctx->origin : ctx->source;
    if (!app_cmd_source_is_entry(entry_source)) {
        return APP_CMD_POLICY_FORBIDDEN;
    }

    kind = app_cmd_kind(method);

    /* Internal apply child: only the commit of an existing operation. */
    if (internal) {
        return kind == APP_CMD_KIND_NET_APPLY ? APP_CMD_POLICY_ALLOW
                                              : APP_CMD_POLICY_FORBIDDEN;
    }

    switch (entry_source) {
    case APP_CMD_SOURCE_MQTT:
        if (kind == APP_CMD_KIND_NET_PROFILE_DELETE || kind == APP_CMD_KIND_NET_SCAN ||
            kind == APP_CMD_KIND_NET_RECONNECT) {
            return APP_CMD_POLICY_FORBIDDEN;
        }
        if (kind == APP_CMD_KIND_NET_TRY || kind == APP_CMD_KIND_NET_APPLY ||
            kind == APP_CMD_KIND_NET_CANCEL) {
            return app_cmd_mqtt_net_txn_enabled() ? APP_CMD_POLICY_ALLOW
                                                  : APP_CMD_POLICY_NOT_ENABLED;
        }
        return APP_CMD_POLICY_ALLOW; /* status / operation.get (owner checked later) */

    case APP_CMD_SOURCE_BLE:
    case APP_CMD_SOURCE_HTTP:
        return (kind == APP_CMD_KIND_NET_PROFILE_DELETE || kind == APP_CMD_KIND_NET_SCAN ||
                kind == APP_CMD_KIND_NET_RECONNECT || kind == APP_CMD_KIND_NET_STATUS ||
                kind == APP_CMD_KIND_NET_TRY || kind == APP_CMD_KIND_NET_APPLY ||
                kind == APP_CMD_KIND_NET_CANCEL || kind == APP_CMD_KIND_OPERATION_GET)
                   ? APP_CMD_POLICY_ALLOW
                   : APP_CMD_POLICY_FORBIDDEN;

    default:
        return APP_CMD_POLICY_FORBIDDEN;
    }
}

app_cmd_policy_result_t app_cmd_policy_check_apply_owner(const app_cmd_context_t *ctx,
                                                         app_operation_owner_t owner)
{
    app_operation_owner_t caller;

    if (ctx == NULL || !app_cmd_ctx_valid(ctx)) {
        return APP_CMD_POLICY_FORBIDDEN;
    }
    caller = app_cmd_ctx_owner(ctx);
    if (caller == APP_OPERATION_OWNER_INTERNAL) {
        return APP_CMD_POLICY_FORBIDDEN;
    }
    return caller == owner ? APP_CMD_POLICY_ALLOW : APP_CMD_POLICY_FORBIDDEN;
}

bool app_cmd_cancel_allowed(const app_cmd_context_t *ctx,
                            app_operation_owner_t owner)
{
    app_cmd_source_t source;

    if (ctx == NULL || !app_cmd_ctx_valid(ctx) ||
        ctx->auth_level != APP_CMD_AUTH_LOCAL_TOKEN) {
        return false;
    }
    if (ctx->exec_env != APP_CMD_EXEC_ENTRY) {
        return false;
    }
    source = ctx->source;

    switch (source) {
    case APP_CMD_SOURCE_BLE:
        return owner == APP_OPERATION_OWNER_BLE || owner == APP_OPERATION_OWNER_MQTT ||
               owner == APP_OPERATION_OWNER_HTTP;
    case APP_CMD_SOURCE_MQTT:
        return owner == APP_OPERATION_OWNER_MQTT;
    case APP_CMD_SOURCE_HTTP:
        return owner == APP_OPERATION_OWNER_HTTP;
    default:
        return false;
    }
}

bool app_cmd_mqtt_net_txn_enabled(void)
{
#if APP_CMD_MQTT_NET_TXN_ENABLE
    return true;
#else
    return false;
#endif
}

const char *app_cmd_policy_build_variant(void)
{
#if APP_CMD_MQTT_NET_TXN_ENABLE
    return "mqtt_net_txn=on";
#else
    return "mqtt_net_txn=off";
#endif
}
