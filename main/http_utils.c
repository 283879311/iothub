#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"
#include "esp_http_server.h"

#include "constants.h"
#include "http_utils.h"

static const char *TAG = "iothub_http";

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
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return http_send_or_ignore_disconnect(httpd_resp_sendstr(req, payload), "json response");
}

esp_err_t app_http_send_jsonf(httpd_req_t *req, const char *status, const char *fmt, ...)
{
    static char payload[APP_JSON_BUFFER_SIZE];
    va_list args;

    va_start(args, fmt);
    vsnprintf(payload, sizeof(payload), fmt, args);
    va_end(args);
    return app_http_send_json_text(req, status, payload);
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
