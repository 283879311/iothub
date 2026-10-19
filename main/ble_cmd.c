#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "ble_cmd.h"
#include "bt_gatts.h"
#include "cmd.h"
#include "http_priv.h"
#include "ota.h"

static const char *TAG = "ble_cmd";

/* 行重组:JSON 行以 \n 结尾(裸字节管道,单写 ≤MTU-3);单行上限 512B。
 * 1.3-87:也接受"一次写入就是完整 JSON 对象"的形式(末尾无换行),
 * 兼容只发送 ASCII 文本、不带行结束符的通用 BLE 串口 App。 */
#define BLE_CMD_LINE_MAX 512
#define BLE_CMD_QUEUE_DEPTH 8
#define BLE_CMD_TOKEN_TTL_MS 600000

typedef struct {
    uint16_t len;
    char buf[BLE_CMD_LINE_MAX + 1];
} ble_cmd_line_t;

static char s_line[BLE_CMD_LINE_MAX + 1];
static size_t s_line_len = 0;
static QueueHandle_t s_cmd_queue = NULL;
static StaticQueue_t s_cmd_queue_buf;
static uint8_t s_cmd_queue_storage[BLE_CMD_QUEUE_DEPTH * sizeof(ble_cmd_line_t)];

/* 短时 token:连接级鉴权(login 用 web 同源口令换 token,后续命令带 token) */
static char s_token[33];
static int64_t s_token_expire_ms = 0;
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
    ble_cmd_line_t line;

    s_line[s_line_len] = '\0';
    line.len = (uint16_t)s_line_len;
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
            /* 行超长:整行丢弃,等下一个换行复位 */
            s_line_len = 0;
            while (i < len && data[i] != '\n') {
                i++;
            }
            ESP_LOGW(TAG, "cmd line too long, dropped");
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
    s_token[0] = '\0';
    s_token_expire_ms = 0;
    s_last_cmd_ms = 0;
    s_line_len = 0;
    ESP_LOGI(TAG, "cmd session token invalidated (disconnect)");
}

static uint32_t s_cmd_notify_fail;

static void app_ble_cmd_send(const cJSON *resp)
{
    char *body;
    bool sent = false;
    int attempt;

    if (resp == NULL) {
        return;
    }
    body = cJSON_PrintUnformatted(resp);
    if (body == NULL) {
        return;
    }
    /* 1.3-76:notify 失败(msys 暂空/拥塞/未订阅)短重试 3 次,10ms 间隔;
     * 官方 NimBLE 的 ble_gatts_notify_custom 没有等待 buffer 的选项,
     * 只能在应用层重试;响应很小,不会出现前缀重复问题。 */
    for (attempt = 0; attempt < 3 && !sent; attempt++) {
        sent = app_ble_gatts_cmd_notify((const uint8_t *)body, strlen(body));
        if (!sent) {
            vTaskDelay(pdMS_TO_TICKS(10));
        }
    }
    if (!sent) {
        s_cmd_notify_fail++;
        if ((s_cmd_notify_fail % 8u) == 1u) {
            ESP_LOGW(TAG, "cmd notify failed after retries (total=%u)",
                     (unsigned)s_cmd_notify_fail);
        }
    }
    cJSON_free(body);
}

static void app_ble_cmd_issue_token(char *out, size_t out_size)
{
    uint8_t raw[16];
    size_t i;

    esp_fill_random(raw, sizeof(raw));
    for (i = 0; i < sizeof(raw) && i * 2 + 2 <= out_size; i++) {
        snprintf(out + i * 2, 3, "%02X", raw[i]);
    }
    s_token_expire_ms = esp_timer_get_time() / 1000 + BLE_CMD_TOKEN_TTL_MS;
}

static bool app_ble_cmd_authed(const cJSON *frame)
{
    const cJSON *tok = cJSON_GetObjectItemCaseSensitive(frame, "token");
    int64_t now = esp_timer_get_time() / 1000;

    if (s_token[0] == '\0' || now >= s_token_expire_ms) {
        return false;
    }
    return cJSON_IsString(tok) && tok->valuestring != NULL &&
           strcmp(tok->valuestring, s_token) == 0;
}

static void app_ble_cmd_handle_line(const char *line, size_t len)
{
    cJSON *frame = NULL;
    cJSON *method = NULL;
    cJSON *params = NULL;
    cJSON *resp = NULL;
    cJSON *id_item = NULL;
    int64_t id = -1;
    bool has_id = false;
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

    id_item = cJSON_GetObjectItemCaseSensitive(frame, "id");
    if (cJSON_IsNumber(id_item)) {
        id = (int64_t)id_item->valuedouble;
        has_id = true;
    }

    method = cJSON_GetObjectItemCaseSensitive(frame, "method");
    params = cJSON_GetObjectItemCaseSensitive(frame, "params");
    if (cJSON_IsString(method) && method->valuestring != NULL) {
        ESP_LOGI(TAG, "rx method=%s id=%lld", method->valuestring, (long long)id);
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
                  app_http_check_credentials(u->valuestring, p->valuestring);

        if (ok) {
            app_ble_cmd_issue_token(s_token, sizeof(s_token));
            /* 只放结果字段;外层统一包装补 {"id","ok","result"},
             * 避免 token 落在 result.result.token。 */
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddStringToObject(resp, "token", s_token);
                cJSON_AddNumberToObject(resp, "expires_in", (double)(BLE_CMD_TOKEN_TTL_MS / 1000));
                ESP_LOGI(TAG, "ble cmd login ok, token issued (ttl=%us)",
                         (unsigned)(BLE_CMD_TOKEN_TTL_MS / 1000));
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

    /* 其余命令:必须先 login 拿 token */
    if (!app_ble_cmd_authed(frame)) {
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
    /* 统一包 {"id":N,"ok":true,"result":{...}} 外壳(错误响应已自带 ok=false) */
    if (has_id && resp != NULL && cJSON_HasObjectItem(resp, "ok")) {
        /* 已含 ok 外壳(错误路径):补 id */
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
        app_ble_cmd_handle_line(line.buf, (size_t)line.len);
    }
}

esp_err_t app_ble_cmd_start(void)
{
    if (s_cmd_queue != NULL) {
        return ESP_OK;
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
