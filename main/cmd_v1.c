#include <string.h>

#include "cmd_v1.h"

#include "cmd_net_adapter.h"
#include "cmd_policy.h"

typedef enum {
    V1_KIND_KERNEL = 0,
    V1_KIND_TRYCONNECT,
    V1_KIND_APPLY,
    V1_KIND_CANCEL,
} app_cmd_v1_kind_t;

/* v0 彻底删除(2026-11-05):不再维护 name → 旧方法别名,表里只有 canonical 名。 */
typedef struct {
    const char *name;
    uint32_t sources;
    app_cmd_v1_kind_t kind;
} app_cmd_v1_spec_t;

#define SRC_BLE  (1u << APP_CMD_SOURCE_BLE)
#define SRC_MQTT (1u << APP_CMD_SOURCE_MQTT)

static const app_cmd_v1_spec_t k_v1_commands[] = {
    {"device.info", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"device.status", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"gpio.status", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"uart.runtime", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"config.get", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"config.set", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"io.get", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"io.set", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"led.set", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"led.toggle", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"relay.set", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"relay.toggle", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"network.status", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"network.scan", SRC_BLE, V1_KIND_KERNEL},
    {"network.profile.list", SRC_BLE, V1_KIND_KERNEL},
    {"network.profile.delete", SRC_BLE, V1_KIND_KERNEL},
    {"network.reconnect", SRC_BLE, V1_KIND_KERNEL},
    {"network.tryconnect", SRC_BLE | SRC_MQTT, V1_KIND_TRYCONNECT},
    {"network.apply", SRC_BLE | SRC_MQTT, V1_KIND_APPLY},
    {"network.cancel", SRC_BLE | SRC_MQTT, V1_KIND_CANCEL},
    {"operation.get", SRC_BLE | SRC_MQTT, V1_KIND_KERNEL},
    {"auth.login", SRC_BLE, V1_KIND_KERNEL},
    {"auth.admin.set", SRC_BLE, V1_KIND_KERNEL},
    {"provisioning.pin.set", SRC_BLE, V1_KIND_KERNEL},
    {"uart.send", SRC_BLE, V1_KIND_KERNEL},
    {"data.loopback", SRC_BLE, V1_KIND_KERNEL},
#ifdef APP_WIFI_FAULT_INJECT
    /* 测试钩子:仅故障注入构建注册,生产镜像不含(M1 故障注入/op_ttl 旋钮入口)。 */
    {"diag.m1hook", SRC_BLE, V1_KIND_KERNEL},
#endif
#ifdef APP_C1_TEST_HOOKS
    /* 测试钩子:仅 C1 实验构建注册(状态机探针)。 */
    {"diag.testhook", SRC_BLE, V1_KIND_KERNEL},
#endif
};

static const app_cmd_v1_spec_t *app_cmd_v1_find(const char *cmd)
{
    size_t i;

    if (cmd == NULL) {
        return NULL;
    }
    for (i = 0; i < sizeof(k_v1_commands) / sizeof(k_v1_commands[0]); i++) {
        if (strcmp(cmd, k_v1_commands[i].name) == 0) {
            return &k_v1_commands[i];
        }
    }
    return NULL;
}

/* 表内命令返回其 canonical 名(供 "命令存在但来源不允许 → forbidden" 判定);
 * 表外返回 NULL。v0 别名已删除,这里不再做任何名字映射。 */
const char *app_cmd_v1_internal_method(const char *cmd)
{
    const app_cmd_v1_spec_t *spec = app_cmd_v1_find(cmd);

    return spec != NULL ? spec->name : NULL;
}

bool app_cmd_v1_allowed(const app_cmd_context_t *ctx, const char *cmd)
{
    const app_cmd_v1_spec_t *spec;

    if (ctx == NULL || !app_cmd_ctx_valid(ctx) || ctx->exec_env != APP_CMD_EXEC_ENTRY) {
        return false;
    }
    spec = app_cmd_v1_find(cmd);
    if (spec == NULL) {
        return false;
    }
    if (ctx->source != APP_CMD_SOURCE_BLE && ctx->source != APP_CMD_SOURCE_MQTT) {
        return false;
    }
    return (spec->sources & (1u << ctx->source)) != 0;
}

bool app_cmd_v1_execute(const app_cmd_context_t *ctx, const char *cmd,
                        const cJSON *args, cJSON **result,
                        app_cmd_result_t *out, const char **err)
{
    const app_cmd_v1_spec_t *spec;
    const cJSON *params = args;
    app_cmd_policy_result_t policy;
    bool ok = false;

    if (result != NULL) {
        *result = NULL;
    }
    if (err != NULL) {
        *err = NULL;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (!app_cmd_v1_allowed(ctx, cmd)) {
        if (err != NULL) {
            *err = "guard_rejected";
        }
        return false;
    }
    spec = app_cmd_v1_find(cmd);
    if (spec == NULL) {
        if (err != NULL) {
            *err = "unknown_command";
        }
        return false;
    }
    if (!cJSON_IsObject(args)) {
        if (err != NULL) {
            *err = "params_required";
        }
        return false;
    }

    if (strcmp(cmd, "config.set") == 0) {
        params = cJSON_GetObjectItemCaseSensitive(args, "patch");
        if (!cJSON_IsObject(params)) {
            if (err != NULL) {
                *err = "params_required";
            }
            return false;
        }
    }

    policy = app_cmd_policy_check_network(ctx, spec->name);
    if (policy != APP_CMD_POLICY_ALLOW) {
        if (err != NULL) {
            *err = policy == APP_CMD_POLICY_BUSY ? "busy"
                 : policy == APP_CMD_POLICY_INVALID_ARGUMENT ? "invalid_argument"
                 : "guard_rejected";
        }
        return false;
    }

    if (spec->kind == V1_KIND_TRYCONNECT) {
        return app_cmd_net_tryconnect_adapter(ctx, args, result, err);
    }
    if (spec->kind == V1_KIND_APPLY) {
        return app_cmd_net_apply_adapter(ctx, args, result, err);
    }
    if (spec->kind == V1_KIND_CANCEL) {
        return app_cmd_net_cancel_adapter(ctx, args, result, err);
    }

    ok = (app_cmd_execute_method(ctx, spec->name, params, result, out, err) == ESP_OK &&
          result != NULL && *result != NULL);
    if (!ok && err != NULL && *err == NULL) {
        *err = "internal_error";
    }
    return ok;
}

bool app_cmd_v1_id_from_root(const cJSON *root, app_cmd_v1_id_t *out)
{
    const cJSON *item;

    if (out == NULL) {
        return false;
    }
    app_cmd_v1_id_reset(out);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return app_cmd_v1_id_set_string(out, item->valuestring);
    }
    if (cJSON_IsNumber(item)) {
        return app_cmd_v1_id_set_number(out, item->valuedouble);
    }
    return false;
}

bool app_cmd_v1_id_add(cJSON *root, const app_cmd_v1_id_t *id)
{
    cJSON *added;

    if (root == NULL || id == NULL) {
        return false;
    }
    switch (id->kind) {
    case APP_CMD_V1_ID_STRING:
        added = cJSON_AddStringToObject(root, "id", id->text);
        break;
    case APP_CMD_V1_ID_NUMBER:
        added = cJSON_AddNumberToObject(root, "id", id->number);
        break;
    case APP_CMD_V1_ID_NONE:
        return true;
    default:
        return false;
    }
    return added != NULL;
}
