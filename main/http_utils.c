#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"
#include "lwip/sockets.h"

#include "common.h"        /* app_mutex_ensure:可重入互斥懒初始化 */
#include "constants.h"
#include "http_utils.h"
#include "json_escape.h"

static const char *TAG = "iothub_http";

/* 1.3-36:多段/分块响应关掉 Nagle。AP+STA 双模下空口 RTT ~200ms,Nagle 会让
 * 每个 chunk 都等上一段的 ACK,20KB 页面流式发送被拖到 15s+;直发即可。 */
void app_http_tcp_nodelay(httpd_req_t *req)
{
    int fd = httpd_req_to_sockfd(req);
    int one = 1;

    if (fd >= 0) {
        (void)setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    }
}

static esp_err_t http_send_or_ignore_disconnect(esp_err_t err, const char *context)
{
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Client disconnected during %s: %s", context, esp_err_to_name(err));
    return ESP_OK;
}

esp_err_t app_http_send_json_text(httpd_req_t *req, const char *status, const char *payload)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    app_http_tcp_nodelay(req);
    return http_send_or_ignore_disconnect(httpd_resp_sendstr(req, payload), "json response");
}

/* ============================================================================
 * 零堆 JSON 构建基建(问题与版本优化.md 1.3-12)
 *
 * 动机:status/network 两个热接口原走 cJSON"建树 + 序列化"(两份堆),在并发 + 堆
 * 偏紧(BLE 常驻时 free 仅 ~16KB)时会 500(out_of_memory)或出现秒级延迟尖峰。
 * 现改为"静态缓冲 + 互斥 + 轮转转义槽":零堆、零栈开销、并发安全。
 *
 * 锁语义:一把**可重入**互斥。调用方若需用 app_json_escaped() 取转义串,
 * 必须在 app_json_build_lock()/unlock() 之间完成"取串 + 拼接 + 发送",
 * 因为轮转槽是在加锁期间共享的。app_http_send_jsonf() 内部自持同一把锁,
 * 故未做转义的老调用点也自动获得并发保护(不会互相覆盖 payload)。
 * ========================================================================== */
#define APP_JSON_ESC_SLOTS     16
#define APP_JSON_ESC_SLOT_SIZE 288   /* 覆盖最长字段(mqtt_host[128] 转义 ×2 + 余量) */

static SemaphoreHandle_t s_json_lock = NULL;
static StaticSemaphore_t s_json_lock_buf;
static portMUX_TYPE s_json_lock_mux = portMUX_INITIALIZER_UNLOCKED;
static char s_json_esc_slots[APP_JSON_ESC_SLOTS][APP_JSON_ESC_SLOT_SIZE];
static unsigned s_json_esc_next = 0;

void app_json_build_lock(void)
{
    xSemaphoreTakeRecursive(app_mutex_ensure(&s_json_lock, &s_json_lock_buf,
                                             &s_json_lock_mux, true), portMAX_DELAY);
    /* 每次(最外层)加锁都从 0 号槽开始:单次请求内 ≤16 个转义串不会互相覆盖。
     * 可重入嵌套(调用方持锁 + send 内部再取)只影响游标,不影响已取到的指针。 */
    s_json_esc_next = 0;
}

void app_json_build_unlock(void)
{
    if (s_json_lock != NULL) {
        xSemaphoreGiveRecursive(s_json_lock);
    }
}

const char *app_json_escaped(const char *src)
{
    char *slot = s_json_esc_slots[s_json_esc_next];

    s_json_esc_next = (s_json_esc_next + 1u) % APP_JSON_ESC_SLOTS;
    app_json_escape_string(slot, APP_JSON_ESC_SLOT_SIZE, (src != NULL) ? src : "");
    return slot;
}

esp_err_t app_http_send_jsonf(httpd_req_t *req, const char *status, const char *fmt, ...)
{
    static char payload[APP_JSON_BUFFER_SIZE];
    va_list args;
    esp_err_t err;
    int need;

    app_json_build_lock();
    va_start(args, fmt);
    need = vsnprintf(payload, sizeof(payload), fmt, args);
    va_end(args);
    if (need < 0 || (size_t)need >= sizeof(payload)) {
        /* 截断即响应不完整:必须告警(字节比对/前端解析都会立刻暴露) */
        ESP_LOGW(TAG, "json response truncated: need=%d cap=%u",
                 need, (unsigned)sizeof(payload));
    }
    err = app_http_send_json_text(req, status, payload);
    app_json_build_unlock();
    return err;
}

static const cJSON *app_json_find_item(const char *json, const char *key)
{
    const cJSON *root;
    const cJSON *item;

    if (json == NULL || key == NULL) {
        return NULL;
    }
    root = cJSON_ParseWithLength(json, strlen(json));
    if (!cJSON_IsObject(root)) {
        cJSON_Delete((cJSON *)root);
        return NULL;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item == NULL) {
        cJSON_Delete((cJSON *)root);
        return NULL;
    }
    return root;
}

static void app_json_find_release(const cJSON *root)
{
    cJSON_Delete((cJSON *)root);
}

bool app_json_find_string(const char *json, const char *key, char *out, size_t out_size)
{
    const cJSON *root;
    const cJSON *item;
    const char *val;
    bool ok = false;

    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    root = app_json_find_item(json, key);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && (val = cJSON_GetStringValue(item)) != NULL) {
        size_t len = strlen(val);
        if (len >= out_size) {
            len = out_size - 1;
        }
        memcpy(out, val, len);
        out[len] = '\0';
        ok = true;
    } else if (cJSON_IsNumber(item)) {
        int n = snprintf(out, out_size, "%g", cJSON_GetNumberValue(item));
        ok = (n > 0 && (size_t)n < out_size);
    } else if (cJSON_IsBool(item)) {
        const char *s = cJSON_IsTrue(item) ? "true" : "false";
        size_t len = strlen(s);
        if (len < out_size) {
            memcpy(out, s, len + 1);
            ok = true;
        }
    }
    app_json_find_release(root);
    return ok;
}

bool app_json_find_bool(const char *json, const char *key, bool *value)
{
    const cJSON *root;
    const cJSON *item;
    bool ok = false;

    if (value == NULL) {
        return false;
    }
    root = app_json_find_item(json, key);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsBool(item)) {
        *value = cJSON_IsTrue(item);
        ok = true;
    } else if (cJSON_IsNumber(item)) {
        double n = cJSON_GetNumberValue(item);
        *value = (n != 0.0);
        ok = true;
    } else if (cJSON_IsString(item)) {
        const char *s = cJSON_GetStringValue(item);
        if (s != NULL && (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
                          strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0)) {
            *value = true;
            ok = true;
        } else if (s != NULL && (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0 ||
                                 strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0)) {
            *value = false;
            ok = true;
        }
    }
    app_json_find_release(root);
    return ok;
}

bool app_json_find_u16(const char *json, const char *key, uint16_t *value)
{
    const cJSON *root;
    const cJSON *item;
    bool ok = false;

    if (value == NULL) {
        return false;
    }
    root = app_json_find_item(json, key);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        double n = cJSON_GetNumberValue(item);
        if (n >= 0.0 && n <= 65535.0) {
            *value = (uint16_t)(n + 0.0);
            ok = true;
        }
    } else if (cJSON_IsString(item)) {
        const char *s = cJSON_GetStringValue(item);
        char *end = NULL;
        long parsed = strtol(s ? s : "", &end, 10);
        if (end != s && parsed >= 0 && parsed <= 65535) {
            *value = (uint16_t)parsed;
            ok = true;
        }
    }
    app_json_find_release(root);
    return ok;
}

bool app_json_find_u32(const char *json, const char *key, uint32_t *value)
{
    const cJSON *root;
    const cJSON *item;
    bool ok = false;

    if (value == NULL) {
        return false;
    }
    root = app_json_find_item(json, key);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        double n = cJSON_GetNumberValue(item);
        if (n >= 0.0 && n <= 4294967295.0) {
            *value = (uint32_t)(n + 0.0);
            ok = true;
        }
    } else if (cJSON_IsString(item)) {
        const char *s = cJSON_GetStringValue(item);
        char *end = NULL;
        unsigned long parsed = strtoul(s ? s : "", &end, 10);
        if (end != s && parsed <= 0xFFFFFFFFUL) {
            *value = (uint32_t)parsed;
            ok = true;
        }
    }
    app_json_find_release(root);
    return ok;
}

bool app_json_find_double(const char *json, const char *key, double *value)
{
    const cJSON *root;
    const cJSON *item;
    bool ok = false;

    if (value == NULL) {
        return false;
    }
    root = app_json_find_item(json, key);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) {
        *value = cJSON_GetNumberValue(item);
        ok = true;
    } else if (cJSON_IsString(item)) {
        const char *s = cJSON_GetStringValue(item);
        char *end = NULL;
        double parsed = strtod(s ? s : "", &end);
        if (end != s) {
            *value = parsed;
            ok = true;
        }
    } else if (cJSON_IsBool(item)) {
        *value = cJSON_IsTrue(item) ? 1.0 : 0.0;
        ok = true;
    }
    app_json_find_release(root);
    return ok;
}
