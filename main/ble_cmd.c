#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ble_cmd.h"
#include "bt_gatts.h"
#include "cmd.h"
#include "config_apply.h"
#include "auth.h"
#include "ota.h"

static const char *TAG = "ble_cmd";

/* 行重组:JSON 行以 \n 结尾(裸字节管道,单写 ≤MTU-3);单行上限 512B。
 * 1.3-87:也接受"一次写入就是完整 JSON 对象"的形式(末尾无换行),
 * 兼容只发送 ASCII 文本、不带行结束符的通用 BLE 串口 App。 */
#define BLE_CMD_LINE_MAX 512
#define BLE_CMD_QUEUE_DEPTH 8
#define BLE_CMD_TX_QUEUE_DEPTH 8
/* token TTL 归 auth_token.c 统一管理(APP_TOKEN_TTL_MS)。 */

#define BLE_CMD_ITEM_LINE  0
#define BLE_CMD_ITEM_ERROR 1

typedef struct {
    uint16_t len;
    uint8_t kind;
    char buf[BLE_CMD_LINE_MAX + 1];
} ble_cmd_line_t;

static char s_line[BLE_CMD_LINE_MAX + 1];
static size_t s_line_len = 0;
static QueueHandle_t s_cmd_queue = NULL;
static StaticQueue_t s_cmd_queue_buf;
static uint8_t s_cmd_queue_storage[BLE_CMD_QUEUE_DEPTH * sizeof(ble_cmd_line_t)];

/* CMD TX 独立发送队列：命令任务只入队，20ms 分片间隔由发送任务承担，
 * 避免长响应期间阻塞 operation.get/取消命令。 */
typedef struct {
    char *body;
    size_t len;
} ble_cmd_tx_msg_t;

static QueueHandle_t s_tx_queue = NULL;
static StaticQueue_t s_tx_queue_buf;
static uint8_t s_tx_queue_storage[BLE_CMD_TX_QUEUE_DEPTH * sizeof(ble_cmd_tx_msg_t)];
static bool s_tx_ready = false;

/* 短时 token:连接级鉴权(login 用 web 同源口令换 token,后续命令带 token)。
 * 槽位与 TTL 已迁到 auth_token.c(按来源分槽、锁内比较);本文件不再持有
 * token 内容,只保留命令限流时间戳。 */
static int64_t s_last_cmd_ms = 0;

/* 判断缓冲区里是否已经有一个完整的 JSON 对象(括号配对,跳过字符串内容)。
 * 仅做字节扫描,不分配内存;用于兼容末尾不带 \n 的 ASCII 写入。 */
static bool app_ble_cmd_json_complete(const char *s, size_t len)
{
    bool started = false;
    bool in_string = false;
    bool escaped = false;
    int depth = 0;
    size_t i;

    for (i = 0; i < len; i++) {
        char ch = s[i];

        if (!started) {
            if (ch == '{') {
                started = true;
                depth = 1;
            }
            continue;
        }
        if (in_string) {
            if (escaped) {
                escaped = false;
            } else if (ch == '\\') {
                escaped = true;
            } else if (ch == '"') {
                in_string = false;
            }
            continue;
        }
        if (ch == '"') {
            in_string = true;
        } else if (ch == '{') {
            depth++;
        } else if (ch == '}') {
            depth--;
            if (depth == 0) {
                return true;
            }
        }
    }
    return false;
}

static void app_ble_cmd_queue_line(void)
{
    ble_cmd_line_t line = {0};

    s_line[s_line_len] = '\0';
    line.len = (uint16_t)s_line_len;
    line.kind = BLE_CMD_ITEM_LINE;
    memcpy(line.buf, s_line, s_line_len + 1);
    if (s_line_len > 0 && s_cmd_queue != NULL) {
        if (xQueueSend(s_cmd_queue, &line, 0) != pdTRUE) {
            ESP_LOGW(TAG, "cmd queue full, line dropped (%u bytes)", (unsigned)s_line_len);
        } else {
            ESP_LOGI(TAG, "cmd line queued (%u bytes)", (unsigned)s_line_len);
        }
    }
    s_line_len = 0;
}

/* 超长命令不能静默丢弃：GATT 回调只投递错误项，由 CMD 任务回包。 */
static void app_ble_cmd_queue_error(const char *error)
{
    ble_cmd_line_t line = {0};

    if (s_cmd_queue == NULL) {
        return;
    }
    line.kind = BLE_CMD_ITEM_ERROR;
    snprintf(line.buf, sizeof(line.buf), "%s", error != NULL ? error : "cmd_error");
    line.len = (uint16_t)strlen(line.buf);
    if (xQueueSend(s_cmd_queue, &line, 0) != pdTRUE) {
        ESP_LOGW(TAG, "cmd queue full, error response dropped (%s)", line.buf);
    }
}

/* CMD 特征写回调:GATT 回调上下文,只做行重组 + 入队,绝不执行命令 */
void app_ble_cmd_on_write(const uint8_t *data, size_t len)
{
    size_t i;
    bool saw_newline = false;

    if (data == NULL || len == 0) {
        return;
    }
    for (i = 0; i < len; i++) {
        uint8_t ch = data[i];

        if (ch == '\n') {
            saw_newline = true;
            app_ble_cmd_queue_line();
            continue;
        }
        if (s_line_len >= BLE_CMD_LINE_MAX) {
            /* 行超长:整行丢弃,等下一个换行复位;必须回一个错误，
             * 否则客户端只能等到超时，无法区分“没收到”和“命令非法”。 */
            s_line_len = 0;
            while (i < len && data[i] != '\n') {
                i++;
            }
            ESP_LOGW(TAG, "cmd line too long, rejected");
            app_ble_cmd_queue_error("line_too_long");
            continue;
        }
        s_line[s_line_len++] = (char)ch;
    }

    /* 本次写入没有换行,但已经凑成完整 JSON 对象 → 直接提交 */
    if (!saw_newline && s_line_len > 0 &&
        app_ble_cmd_json_complete(s_line, s_line_len)) {
        ESP_LOGI(TAG, "cmd frame complete without newline (%u bytes)",
                 (unsigned)s_line_len);
        app_ble_cmd_queue_line();
    }
}

void app_ble_cmd_on_disconnect(void)
{
    /* 只清 BLE 槽:串口槽属于另一条会话,断 BLE 不该连带失效。
     * s_last_cmd_ms / 行缓冲 / TX 队列 / network candidate 清理保持原样,
     * 它们不是 token 语义的一部分(A 阶段不扩大范围)。 */
    app_token_store_clear(TOKEN_SRC_BLE);
    s_last_cmd_ms = 0;
    s_line_len = 0;
    if (s_tx_queue != NULL) {
        ble_cmd_tx_msg_t msg;
        while (xQueueReceive(s_tx_queue, &msg, 0) == pdTRUE) {
            cJSON_free(msg.body);
        }
    }
    app_cmd_net_on_ble_disconnect();
    ESP_LOGI(TAG, "cmd session token invalidated (disconnect)");
}

static uint32_t s_cmd_notify_fail;

static void app_ble_cmd_tx_one(const char *body, size_t body_len)
{
    bool complete = false;
    bool partial = false;
    int partial_bytes = 0;
    int attempt;

    /* 仅在“一个字节都没送出”时重试(msys 暂空/拥塞/未订阅)。
     * 已送出前缀后不重试：从头重发会在客户端造成重复/拼接 JSON；
     * 也不能把部分发送当完整成功，记录 truncated 计数供诊断。 */
    for (attempt = 0; attempt < 3 && !complete; attempt++) {
        int sent = app_ble_gatts_cmd_notify((const uint8_t *)body, body_len);

        if (sent >= 0 && (size_t)sent == body_len) {
            complete = true;
            break;
        }
        if (sent > 0) {
            partial = true;
            partial_bytes = sent;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(10));
    }
    if (!complete) {
        s_cmd_notify_fail++;
        if ((s_cmd_notify_fail % 8u) == 1u) {
            if (partial) {
                ESP_LOGW(TAG, "cmd notify truncated: sent=%d/%u (total=%u)",
                         partial_bytes, (unsigned)body_len, (unsigned)s_cmd_notify_fail);
            } else {
                ESP_LOGW(TAG, "cmd notify failed after retries (total=%u)",
                         (unsigned)s_cmd_notify_fail);
            }
        }
    }
}

static void app_ble_cmd_tx_task(void *arg)
{
    ble_cmd_tx_msg_t msg;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_tx_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        app_ble_cmd_tx_one(msg.body, msg.len);
        cJSON_free(msg.body);
    }
}

static void app_ble_cmd_send(const cJSON *resp)
{
    char *body;
    size_t body_len;
    ble_cmd_tx_msg_t msg;

    if (resp == NULL) {
        return;
    }
    body = cJSON_PrintUnformatted(resp);
    if (body == NULL) {
        return;
    }
    body_len = strlen(body);
    msg.body = body;
    msg.len = body_len;

    if (s_tx_ready && s_tx_queue != NULL &&
        xQueueSend(s_tx_queue, &msg, pdMS_TO_TICKS(50)) == pdTRUE) {
        return;
    }
    if (!s_tx_ready) {
        /* TX 任务未就绪时退回同步发送，保证命令仍有响应。 */
        app_ble_cmd_tx_one(body, body_len);
    } else {
        ESP_LOGW(TAG, "cmd tx queue full, response dropped");
    }
    cJSON_free(body);
}

/* 签发走 store(锁/槽位/TTL 都在那里),这里只固定 BLE 来源。
 * out 由调用方给局部缓冲,回包直接用它 —— 不回读槽位。 */
static bool app_ble_cmd_issue_token(char *out, size_t out_size)
{
    return app_token_store_issue(TOKEN_SRC_BLE, out, out_size);
}

static bool app_ble_cmd_authed(const cJSON *frame)
{
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(frame, "token");

    /* 格式检查与定长比较都在 store 内完成,这里不再做字符串比较。 */
    return cJSON_IsString(tok) && tok->valuestring != NULL &&
           app_token_store_validate(TOKEN_SRC_BLE, tok->valuestring);
}

/* v1.0 适配层：只暴露已有 handler，未实现的网络事务/operation 不伪造。 */
static bool app_ble_cmd_v1_allowed(const char *cmd)
{
    static const char *const allowed[] = {
        "device.info",
        "auth.login",
        "config.get",
        "config.set",
        "network.status",
        "network.scan",
        "network.tryconnect",
        "network.apply",
        "network.cancel",
        "operation.get",
        "network.profile.list",
        "network.profile.delete",
        "led.set",
        "led.toggle",
        "relay.set",
        "relay.toggle",
        "io.set",
        "uart.send",
        "data.loopback",
        "auth.admin.set",
        "provisioning.pin.set",
    };
    size_t i;

    if (cmd == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(allowed) / sizeof(allowed[0]); i++) {
        if (strcmp(cmd, allowed[i]) == 0) {
            return true;
        }
    }
    return false;
}

static const char *app_ble_cmd_v1_map(const char *cmd)
{
    if (strcmp(cmd, "network.tryconnect") == 0) {
        return "network.tryconnect.async";
    }
    if (strcmp(cmd, "network.apply") == 0) {
        return "network.apply.async";
    }
    if (strcmp(cmd, "network.cancel") == 0) {
        return "network.cancel.async";
    }
    if (strcmp(cmd, "auth.login") == 0) {
        return "login";
    }
    if (strcmp(cmd, "network.profile.list") == 0) {
        return "network.profiles";
    }
    if (strcmp(cmd, "auth.admin.set") == 0) {
        return "web.auth.set";
    }
    return cmd;
}

static const char *app_ble_cmd_v1_error_code(const char *v0)
{
    if (v0 == NULL) {
        return "internal_error";
    }
    if (strcmp(v0, "unknown_command") == 0 ||
        strcmp(v0, "unknown_method") == 0) {
        return "unknown_command";
    }
    if (strcmp(v0, "line_too_long") == 0) {
        return "line_too_long";
    }
    if (strcmp(v0, "invalid_json") == 0) {
        return "invalid_json";
    }
    if (strcmp(v0, "unsupported_version") == 0) {
        return "unsupported_version";
    }
    if (strcmp(v0, "params_required") == 0 ||
        strcmp(v0, "invalid_config") == 0 ||
        strcmp(v0, "invalid_param") == 0 ||
        strcmp(v0, "invalid_argument") == 0 ||
        strcmp(v0, "invalid_old_password") == 0 ||
        strcmp(v0, "domain_not_supported") == 0 ||
        strcmp(v0, "password_too_short") == 0) {
        return "invalid_argument";
    }
    if (strcmp(v0, "auth_required") == 0 ||
        strcmp(v0, "invalid_credentials") == 0 ||
        strcmp(v0, "unauthorized") == 0) {
        return "unauthorized";
    }
    if (strcmp(v0, "guard_rejected") == 0 ||
        strcmp(v0, "forbidden") == 0) {
        return "forbidden";
    }
    if (strcmp(v0, "busy") == 0) {
        return "busy";
    }
    if (strcmp(v0, "conflict") == 0) {
        return "conflict";
    }
    if (strcmp(v0, "rate_limited") == 0) {
        return "rate_limited";
    }
    if (strcmp(v0, "internal_error") == 0) {
        return "internal_error";
    }
    if (strcmp(v0, "not_found") == 0 ||
        strcmp(v0, "operation_state") == 0 ||
        strcmp(v0, "trial_invalid") == 0 ||
        strcmp(v0, "wrong_password") == 0 ||
        strcmp(v0, "ap_not_found") == 0 ||
        strcmp(v0, "connect_failed") == 0 ||
        strcmp(v0, "wifi_scan_failed") == 0 ||   /* P1-10:保持可分辨,不折叠进 network_failed */
        strcmp(v0, "no_slot") == 0 ||
        strcmp(v0, "expired") == 0 ||
        strcmp(v0, "rollback_failed") == 0) {
        return v0;
    }
    if (strcmp(v0, "save_failed") == 0 ||
        strcmp(v0, "storage_failed") == 0) {
        return "storage_failed";
    }
    if (strcmp(v0, "wifi_scan_failed") == 0 ||
        strcmp(v0, "network_failed") == 0) {
        return "network_failed";
    }
    if (strcmp(v0, "restart_failed") == 0 ||
        strcmp(v0, "apply_failed") == 0) {
        return "apply_failed";
    }
    return "internal_error";
}

static void app_ble_cmd_wrap_v1(cJSON **resp, const char *id, bool has_id)
{
    cJSON *r = resp != NULL ? *resp : NULL;
    cJSON *outer;
    const cJSON *ok_item;

    if (r == NULL) {
        r = cJSON_CreateObject();
        if (r != NULL) {
            cJSON_AddBoolToObject(r, "ok", false);
            cJSON_AddStringToObject(r, "error", "internal_error");
        }
    }
    if (r == NULL) {
        return;
    }

    ok_item = cJSON_GetObjectItemCaseSensitive(r, "ok");
    if (cJSON_IsBool(ok_item) && !cJSON_IsTrue(ok_item)) {
        const cJSON *err_item = cJSON_GetObjectItemCaseSensitive(r, "error");
        const char *v0 = cJSON_IsString(err_item) ? err_item->valuestring : "internal_error";
        cJSON *err_obj = cJSON_CreateObject();
        cJSON *error = cJSON_CreateObject();

        if (err_obj == NULL || error == NULL) {
            cJSON_Delete(err_obj);
            cJSON_Delete(error);
            cJSON_Delete(r);
            *resp = NULL;
            return;
        }
        if (has_id) {
            cJSON_AddStringToObject(err_obj, "id", id != NULL ? id : "");
        }
        cJSON_AddBoolToObject(err_obj, "ok", false);
        cJSON_AddStringToObject(error, "code", app_ble_cmd_v1_error_code(v0));
        cJSON_AddStringToObject(error, "message", v0);
        cJSON_AddItemToObject(err_obj, "error", error);
        cJSON_Delete(r);
        *resp = err_obj;
        return;
    }

    outer = cJSON_CreateObject();
    if (outer == NULL) {
        cJSON_Delete(r);
        *resp = NULL;
        return;
    }
    if (has_id) {
        cJSON_AddStringToObject(outer, "id", id != NULL ? id : "");
    }
    cJSON_AddBoolToObject(outer, "ok", true);
    cJSON_AddItemToObject(outer, "data", r);
    *resp = outer;
}

static void app_ble_cmd_handle_line(const char *line, size_t len)
{
    cJSON *frame = NULL;
    cJSON *method = NULL;
    cJSON *params = NULL;
    cJSON *resp = NULL;
    cJSON *id_item = NULL;
    const cJSON *version_item = NULL;
    const cJSON *cmd_item = NULL;
    const cJSON *args_item = NULL;
    int64_t id = -1;
    bool has_id = false;
    bool v1_envelope = false;
    bool v1 = false;
    bool v1_has_id = false;
    char v1_id[65] = {0};
    app_cmd_result_t out = {0};
    const char *err = NULL;
    bool reboot_after_response = false;

    frame = cJSON_ParseWithLength(line, len);
    if (frame == NULL || !cJSON_IsObject(frame)) {
        resp = cJSON_CreateObject();
        if (resp != NULL) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "invalid_json");
        }
        app_ble_cmd_send(resp);
        goto done;
    }

    version_item = cJSON_GetObjectItemCaseSensitive(frame, "version");
    if (cJSON_IsString(version_item) && version_item->valuestring != NULL) {
        v1_envelope = true;
        v1 = strcmp(version_item->valuestring, "1.0") == 0;
    }

    id_item = cJSON_GetObjectItemCaseSensitive(frame, "id");
    if (v1_envelope) {
        if (cJSON_IsString(id_item) && id_item->valuestring != NULL) {
            snprintf(v1_id, sizeof(v1_id), "%s", id_item->valuestring);
            v1_has_id = true;
        } else if (cJSON_IsNumber(id_item)) {
            snprintf(v1_id, sizeof(v1_id), "%lld", (long long)id_item->valuedouble);
            v1_has_id = true;
        }
    }
    if (!v1_envelope && cJSON_IsNumber(id_item)) {
        id = (int64_t)id_item->valuedouble;
        has_id = true;
    }

    if (v1_envelope) {
        if (!v1) {
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "unsupported_version");
            }
            goto wrap;
        }
        if (!v1_has_id) {
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "invalid_argument");
            }
            goto wrap;
        }
        cmd_item = cJSON_GetObjectItemCaseSensitive(frame, "cmd");
        args_item = cJSON_GetObjectItemCaseSensitive(frame, "args");
        if (!cJSON_IsString(cmd_item) || cmd_item->valuestring == NULL ||
            !cJSON_IsObject(args_item) ||
            !app_ble_cmd_v1_allowed(cmd_item->valuestring)) {
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error",
                                        (cJSON_IsString(cmd_item) &&
                                         app_ble_cmd_v1_allowed(cmd_item->valuestring))
                                            ? "invalid_argument" : "unknown_command");
            }
            goto wrap;
        }

        {
            const char *mapped = app_ble_cmd_v1_map(cmd_item->valuestring);
            cJSON *normalized = NULL;

            if (strcmp(cmd_item->valuestring, "config.set") == 0) {
                const cJSON *patch = cJSON_GetObjectItemCaseSensitive(args_item, "patch");

                if (!cJSON_IsObject(patch)) {
                    resp = cJSON_CreateObject();
                    if (resp != NULL) {
                        cJSON_AddBoolToObject(resp, "ok", false);
                        cJSON_AddStringToObject(resp, "error", "invalid_argument");
                    }
                    goto wrap;
                }
                if (cJSON_GetObjectItemCaseSensitive(args_item, "idempotency_key") != NULL) {
                    resp = cJSON_CreateObject();
                    if (resp != NULL) {
                        cJSON_AddBoolToObject(resp, "ok", false);
                        cJSON_AddStringToObject(resp, "error", "invalid_argument");
                    }
                    goto wrap;
                }
                {
                    const cJSON *item = NULL;

                    cJSON_ArrayForEach(item, patch) {
                        const cfg_field_t *f = config_apply_field(
                            item->string != NULL ? item->string : "");

                        if (f != NULL && f->domain == CFG_DOMAIN_NET) {
                            resp = cJSON_CreateObject();
                            if (resp != NULL) {
                                cJSON_AddBoolToObject(resp, "ok", false);
                                cJSON_AddStringToObject(resp, "error", "invalid_argument");
                            }
                            goto wrap;
                        }
                    }
                }
                normalized = cJSON_Duplicate(patch, 1);
            } else {
                normalized = cJSON_Duplicate(args_item, 1);
            }
            if (normalized == NULL) {
                resp = cJSON_CreateObject();
                if (resp != NULL) {
                    cJSON_AddBoolToObject(resp, "ok", false);
                    cJSON_AddStringToObject(resp, "error", "internal_error");
                }
                goto wrap;
            }
            cJSON_DeleteItemFromObjectCaseSensitive(frame, "method");
            cJSON_DeleteItemFromObjectCaseSensitive(frame, "params");
            cJSON_AddStringToObject(frame, "method", mapped);
            cJSON_AddItemToObject(frame, "params", normalized);
            method = cJSON_GetObjectItemCaseSensitive(frame, "method");
            params = cJSON_GetObjectItemCaseSensitive(frame, "params");
            ESP_LOGI(TAG, "rx v1 cmd=%s id=%s", cmd_item->valuestring, v1_id);
        }
    } else {
        method = cJSON_GetObjectItemCaseSensitive(frame, "method");
        params = cJSON_GetObjectItemCaseSensitive(frame, "params");
        if (cJSON_IsString(method) && method->valuestring != NULL) {
            ESP_LOGI(TAG, "rx method=%s id=%lld", method->valuestring, (long long)id);
        }
    }

    /* 限频:5 次/秒(与 MQTT RPC 同决策,2026-10-15)。
     * 超限也要回包,否则发送方会把"被丢弃"误判成"BLE 没有响应"。 */
    {
        int64_t now = esp_timer_get_time() / 1000;

        if (now - s_last_cmd_ms < 200) {
            ESP_LOGW(TAG, "ble cmd rate limited");
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "rate_limited");
            }
            goto wrap;
        }
        s_last_cmd_ms = now;
    }

    /* login:web 同源口令换短时 token(免 token) */
    if (cJSON_IsString(method) && method->valuestring != NULL &&
        strcmp(method->valuestring, "login") == 0) {
        const cJSON *u = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "username") : NULL;
        const cJSON *p = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "password") : NULL;
        bool ok = cJSON_IsString(u) && cJSON_IsString(p) &&
                  app_auth_check_credentials(u->valuestring, p->valuestring);

        if (ok) {
            char token[APP_TOKEN_HEX_LEN + 1];

            /* 正常路径不可能失败(out_size 固定 33、来源为常量);但失败必须
             * 有确定出口,否则会退化成"回包 token 为空"的老 bug。 */
            if (!app_ble_cmd_issue_token(token, sizeof(token))) {
                resp = cJSON_CreateObject();
                if (resp != NULL) {
                    cJSON_AddBoolToObject(resp, "ok", false);
                    cJSON_AddStringToObject(resp, "error", "internal_error");
                }
                ESP_LOGE(TAG, "ble cmd login: token issue failed");
            } else {
                /* 只放结果字段;外层统一包装补 {"id","ok","result"},
                 * 避免 token 落在 result.result.token。 */
                resp = cJSON_CreateObject();
                if (resp != NULL) {
                    cJSON_AddStringToObject(resp, "token", token);
                    cJSON_AddNumberToObject(resp, "expires_in",
                                            (double)(APP_TOKEN_TTL_MS / 1000));
                    ESP_LOGI(TAG, "ble cmd login ok, token issued (ttl=%us)",
                             (unsigned)(APP_TOKEN_TTL_MS / 1000));
                }
            }
        } else {
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", "invalid_credentials");
            }
            ESP_LOGW(TAG, "ble cmd login failed");
        }
        goto wrap;
    }

    /* 其余命令:必须先 login 拿 token。device.info 是 v1 无 token 探测入口。 */
    if (!(v1_envelope && cmd_item != NULL && cJSON_IsString(cmd_item) &&
          strcmp(cmd_item->valuestring, "device.info") == 0) &&
        !app_ble_cmd_authed(frame)) {
        resp = cJSON_CreateObject();
        if (resp != NULL) {
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", "auth_required");
        }
        ESP_LOGW(TAG, "ble cmd rejected: auth_required");
        goto wrap;
    }

    /* 命令内核(与 MQTT RPC 同一分发器) */
    if (app_cmd_execute_ex(frame, APP_CMD_SOURCE_BLE, &resp, &out, &err) != ESP_OK || resp == NULL) {
        cJSON *err_resp = cJSON_CreateObject();

        if (err_resp != NULL) {
            cJSON_AddBoolToObject(err_resp, "ok", false);
            cJSON_AddStringToObject(err_resp, "error", err != NULL ? err : "internal_error");
        }
        if (resp != NULL) {
            cJSON_Delete(resp);
        }
        resp = err_resp;
        goto wrap;
    }
    reboot_after_response = out.reboot_after_response;

wrap:
    if (v1_envelope) {
        app_ble_cmd_wrap_v1(&resp, v1_id, v1_has_id);
    } else if (has_id && resp != NULL && cJSON_HasObjectItem(resp, "ok")) {
        /* v0:已含 ok 外壳(错误路径):补 id */
        if (cJSON_GetObjectItemCaseSensitive(resp, "id") == NULL) {
            cJSON_AddNumberToObject(resp, "id", (double)id);
        }
    } else if (resp != NULL) {
        cJSON *outer = cJSON_CreateObject();

        if (outer != NULL) {
            cJSON_AddNumberToObject(outer, "id", (double)id);
            cJSON_AddBoolToObject(outer, "ok", true);
            cJSON_AddItemToObject(outer, "result", resp);   /* 移交所有权 */
            resp = outer;
        }
    }
    app_ble_cmd_send(resp);

    if (reboot_after_response) {
        app_reboot_schedule("ble_cmd_reboot");
        ESP_LOGI(TAG, "ble cmd effect=reboot: response sent, reboot scheduled (1.5s)");
    }

done:
    if (resp != NULL) {
        cJSON_Delete(resp);
    }
    if (frame != NULL) {
        cJSON_Delete(frame);
    }
}

static void app_ble_cmd_task(void *arg)
{
    ble_cmd_line_t line;

    (void)arg;
    for (;;) {
        if (xQueueReceive(s_cmd_queue, &line, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (line.kind == BLE_CMD_ITEM_ERROR) {
            cJSON *resp = cJSON_CreateObject();

            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "ok", false);
                cJSON_AddStringToObject(resp, "error", line.buf);
                app_ble_cmd_send(resp);
                cJSON_Delete(resp);
            }
            continue;
        }
        app_ble_cmd_handle_line(line.buf, (size_t)line.len);
    }
}

esp_err_t app_ble_cmd_start(void)
{
    if (s_cmd_queue != NULL) {
        return ESP_OK;
    }
    if (s_tx_queue == NULL) {
        s_tx_queue = xQueueCreateStatic(BLE_CMD_TX_QUEUE_DEPTH, sizeof(ble_cmd_tx_msg_t),
                                        s_tx_queue_storage, &s_tx_queue_buf);
        if (s_tx_queue == NULL) {
            ESP_LOGW(TAG, "cmd tx queue create failed; using synchronous responses");
        } else if (xTaskCreate(app_ble_cmd_tx_task, "ble_cmd_tx", 3072, NULL, 3, NULL) != pdPASS) {
            ESP_LOGW(TAG, "cmd tx task create failed; using synchronous responses");
        } else {
            s_tx_ready = true;
        }
    }
    s_cmd_queue = xQueueCreateStatic(BLE_CMD_QUEUE_DEPTH, sizeof(ble_cmd_line_t),
                                     s_cmd_queue_storage, &s_cmd_queue_buf);
    if (s_cmd_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    if (xTaskCreate(app_ble_cmd_task, "ble_cmd", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "ble cmd shell ready (line=%u, queue=%u)",
             (unsigned)BLE_CMD_LINE_MAX, (unsigned)BLE_CMD_QUEUE_DEPTH);
    return ESP_OK;
}
