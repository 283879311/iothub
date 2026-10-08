#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_check.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_random.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mbedtls/base64.h"

#include "common.h"
#include "config.h"
#include "constants.h"
#include "http.h"
#include "http_priv.h"
#include "http_utils.h"
#include "identity.h"
#include "mqtt.h"
#include "ota.h"
#include "state.h"
#include "status.h"
#include "uart.h"
#include "wifi.h"

typedef struct {
    httpd_handle_t server;
    char scratch[APP_SCRATCH_SIZE];
} web_context_t;

static const char *TAG = "iothub";
static web_context_t s_web;

char *app_http_scratch_buf(void)
{
    return s_web.scratch;
}

size_t app_http_scratch_size(void)
{
    return sizeof(s_web.scratch);
}

esp_err_t app_http_ignore_client_disconnect(esp_err_t err, const char *context)
{
    if (err == ESP_OK) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "Client disconnected during %s: %s", context, esp_err_to_name(err));
    return ESP_OK;
}

static app_http_route_t s_registered_routes[APP_HTTP_MAX_HANDLERS];
static size_t s_registered_count = 0;

static bool route_already_seen(const char *uri, httpd_method_t method)
{
    size_t i;
    for (i = 0; i < s_registered_count; i++) {
        if (s_registered_routes[i].method == method &&
            strcmp(s_registered_routes[i].uri, uri) == 0) {
            return true;
        }
    }
    return false;
}

esp_err_t app_http_register_uri_handler(httpd_handle_t server,
                                        const char *uri,
                                        httpd_method_t method,
                                        esp_err_t (*handler)(httpd_req_t *req))
{
    if (route_already_seen(uri, method)) {
        return ESP_OK;
    }
    httpd_uri_t cfg = {
        .uri = uri,
        .method = method,
        .handler = handler,
    };
    esp_err_t err = httpd_register_uri_handler(server, &cfg);
    if (err == ESP_OK || err == ESP_ERR_HTTPD_HANDLER_EXISTS) {
        if (!route_already_seen(uri, method) &&
            s_registered_count < APP_HTTP_MAX_HANDLERS) {
            s_registered_routes[s_registered_count].uri = uri;
            s_registered_routes[s_registered_count].method = method;
            s_registered_routes[s_registered_count].handler = handler;
            s_registered_count++;
        }
        return ESP_OK;
    }
    if (s_registered_count >= APP_HTTP_MAX_HANDLERS) {
        const char *mstr = method == HTTP_GET ? "GET" :
                          method == HTTP_POST ? "POST" :
                          method == HTTP_PUT ? "PUT" :
                          method == HTTP_DELETE ? "DELETE" : "OTHER";
        ESP_LOGE(TAG, "registered routes table full (%u), cannot register %s %s",
                 (unsigned)s_registered_count, mstr, uri);
        return ESP_ERR_NO_MEM;
    }
    {
        const char *mstr = method == HTTP_GET ? "GET" :
                          method == HTTP_POST ? "POST" :
                          method == HTTP_PUT ? "PUT" :
                          method == HTTP_DELETE ? "DELETE" : "OTHER";
        ESP_LOGE(TAG, "register %s %s failed: %s", mstr, uri, esp_err_to_name(err));
    }
    return err;
}

/* 校验口令:SHA-256(盐||口令) 与配置中的哈希比较。明文口令不落在固件或 NVS。 */
static bool app_http_verify_password(const char *password)
{
    uint8_t salt[16];
    uint8_t stored[32];
    uint8_t calc[32];

    app_config_lock();
    memcpy(salt, s_config.web_pass_salt, sizeof(salt));
    memcpy(stored, s_config.web_pass_hash, sizeof(stored));
    app_config_unlock();

    app_web_hash_password(salt, sizeof(salt), password, calc);
    /* 常数时间比较:按字节差值累加,避免 memcmp 前缀短路泄漏时序信息 */
    {
        volatile uint8_t diff = 0;
        size_t i;
        for (i = 0; i < sizeof(calc); i++) {
            diff |= (uint8_t)(calc[i] ^ stored[i]);
        }
        return diff == 0;
    }
}

static bool app_http_is_authorized(httpd_req_t *req)
{
    size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    char auth_header[80];
    unsigned char decoded[128];
    size_t decoded_len = 0;
    const char *separator;
    char username[33];
    char stored_username[sizeof(username)];
    size_t user_len;

    if (header_len == 0 || header_len >= sizeof(auth_header)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_header, "Basic ", 6) != 0) {
        return false;
    }
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                              (const unsigned char *)auth_header + 6,
                              strlen(auth_header) - 6) != 0 ||
        decoded_len == 0 || decoded_len >= sizeof(decoded)) {
        return false;
    }
    decoded[decoded_len] = '\0';
    separator = memchr(decoded, ':', decoded_len);
    if (separator == NULL) {
        return false;
    }
    user_len = (size_t)(separator - (const char *)decoded);
    if (user_len == 0 || user_len >= sizeof(username)) {
        return false;
    }
    memcpy(username, decoded, user_len);
    username[user_len] = '\0';

    app_config_lock();
    app_copy_string(stored_username, sizeof(stored_username), s_config.web_username);
    app_config_unlock();
    if (stored_username[0] == '\0' || strcmp(username, stored_username) != 0) {
        return false;
    }
    return app_http_verify_password(separator + 1);
}

esp_err_t app_http_send_auth_challenge(httpd_req_t *req)
{
    char auth_header[64];

    snprintf(auth_header, sizeof(auth_header), "Basic realm=\"%s\"", APP_WEB_AUTH_REALM);
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "WWW-Authenticate", auth_header);
    if (strncmp(req->uri, "/api/", 5) == 0) {
        return app_http_send_json_text(req, "401 Unauthorized",
                                   "{\"status\":\"error\",\"message\":\"auth_required\"}");
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    return app_http_ignore_client_disconnect(httpd_resp_sendstr(req, "Authentication required"),
                                             "auth challenge");
}

bool app_http_require_auth(httpd_req_t *req)
{
    if (app_http_is_authorized(req)) {
        return true;
    }
    app_http_send_auth_challenge(req);
    return false;
}

bool app_http_body_read_finished(esp_err_t err)
{
    return err == ESP_ERR_INVALID_STATE;
}

esp_err_t http_read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int remaining = req->content_len;
    int received = 0;

    if (remaining >= (int)buf_len) {
        return app_http_send_json_text(req, "413 Payload Too Large",
                                   "{\"status\":\"error\",\"message\":\"payload_too_large\"}");
    }

    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret == 0 || ret == HTTPD_SOCK_ERR_TIMEOUT || ret == HTTPD_SOCK_ERR_FAIL) {
            app_http_ignore_client_disconnect(ESP_FAIL, "request body");
            return ESP_ERR_INVALID_STATE;
        }
        if (ret == HTTPD_SOCK_ERR_INVALID) {
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"body_read_failed\"}");
        }
        received += ret;
        remaining -= ret;
    }

    buf[received] = '\0';
    return ESP_OK;
}

/* 页面资产构建期已 gzip 预压缩,按 Accept-Encoding 决定能否出 .gz */
static bool http_accepts_gzip(httpd_req_t *req)
{
    char value[128];
    size_t len = httpd_req_get_hdr_value_len(req, "Accept-Encoding");
    size_t i;

    if (len == 0 || len >= sizeof(value)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Accept-Encoding", value, sizeof(value)) != ESP_OK) {
        return false;
    }
    for (i = 0; i < len; i++) {
        value[i] = (char)tolower((unsigned char)value[i]);
    }
    return strstr(value, "gzip") != NULL;
}

esp_err_t http_serve_html(httpd_req_t *req, const char *file_name)
{
    char path[96];
    FILE *file = NULL;
    char chunk[1024];
    size_t read_bytes = 0;
    bool accepts_gzip = http_accepts_gzip(req);
    bool use_gzip = false;

    /* 管理页认证按文件级统一控制,新增路由只要经此函数出 config.html 就自动覆盖 */
    if (strcmp(file_name, "config.html") == 0 && !app_http_require_auth(req)) {
        return ESP_OK;
    }

    /* 优先出 .gz;旧布局 storage.bin 只有未压缩文件时自动回落 */
    if (accepts_gzip) {
        snprintf(path, sizeof(path), "%s/%s.gz", APP_BASE_PATH, file_name);
        file = fopen(path, "rb");
        use_gzip = file != NULL;
    }
    if (file == NULL) {
        snprintf(path, sizeof(path), "%s/%s", APP_BASE_PATH, file_name);
        file = fopen(path, "rb");
    }
    if (file == NULL) {
        if (!accepts_gzip) {
            return app_http_send_json_text(req, "406 Not Acceptable",
                                       "{\"status\":\"error\",\"message\":\"gzip_required\"}");
        }
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"page_open_failed\"}");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    if (use_gzip) {
        httpd_resp_set_hdr(req, "Content-Encoding", "gzip");
    }
    do {
        read_bytes = fread(chunk, 1, sizeof(chunk), file);
        if (read_bytes > 0) {
            esp_err_t err = httpd_resp_send_chunk(req, chunk, read_bytes);
            if (err != ESP_OK) {
                fclose(file);
                return app_http_ignore_client_disconnect(err, "html chunk");
            }
        }
    } while (read_bytes > 0);

    fclose(file);
    return app_http_ignore_client_disconnect(httpd_resp_send_chunk(req, NULL, 0),
                                             "html terminator");
}

static esp_err_t device_info_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_status_send_device_info(req);
}

static esp_err_t device_status_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_status_send_device_status(req);
}

static bool app_uri_ends_with(const char *uri, size_t uri_len, const char *suffix)
{
    size_t suffix_len = strlen(suffix);

    return uri_len >= suffix_len &&
           memcmp(uri + uri_len - suffix_len, suffix, suffix_len) == 0;
}

/* AP 模式强制门户:各 OS 的连通性探测端点统一 302 到落地页。
 * 探测发生在登录前,故不要求认证 */
static bool app_http_is_captive_probe(const char *uri)
{
    /* 按后缀匹配:各厂商探测域名繁多(MIUI/华为/vivo 等),路径相同只换域名 */
    static const char *const probes[] = {
        "generate_204",                 /* 安卓各厂商 */
        "gen_204",
        "hotspot-detect.html",          /* iOS/macOS */
        "library/test/success.html",    /* 老版本 iOS */
        "connecttest.txt",              /* Windows 10/11 */
        "ncsi.txt",                     /* 老版本 Windows */
        "success.txt",                  /* Firefox */
    };
    const char *path = uri;
    size_t path_len;
    size_t i;

    /* 兼容 absolute-form 请求行(GET http://host/path):跳过 scheme+authority */
    {
        const char *scheme = strstr(uri, "://");
        if (scheme != NULL) {
            const char *slash = strchr(scheme + 3, '/');
            path = slash != NULL ? slash : "/";
        }
    }
    /* 探测可能带 query string,截断后再匹配 */
    path_len = strlen(path);
    {
        const char *query = memchr(path, '?', path_len);
        if (query != NULL) {
            path_len = (size_t)(query - path);
        }
    }
    for (i = 0; i < sizeof(probes) / sizeof(probes[0]); i++) {
        if (app_uri_ends_with(path, path_len, probes[i])) {
            return true;
        }
    }
    return false;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *file_name = NULL;
    char host[48];

    /* 全量请求日志:外场定位强制门户问题(此前 401/404 分支静默,手机行为不可见) */
    if (httpd_req_get_hdr_value_str(req, "Host", host, sizeof(host)) != ESP_OK) {
        host[0] = '\0';
    }
    ESP_LOGI(TAG, "http get %s (host=%s)", req->uri, host[0] != '\0' ? host : "-");

    if (app_http_is_captive_probe(req->uri)) {
        /* 302 目标必须免认证 200:CNA 弹窗无法渲染 Basic 挑战,指到 / 会 401 关窗 */
        ESP_LOGI(TAG, "captive probe %s -> 302 " APP_WEB_PORTAL_PATH, req->uri);
        httpd_resp_set_status(req, "302 Found");
        httpd_resp_set_hdr(req, "Cache-Control", "no-store");
        httpd_resp_set_hdr(req, "Location", "http://" APP_AP_GATEWAY_IP APP_WEB_PORTAL_PATH);
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, "Redirect to captive portal");
    }

    /* 单页形态:所有页面入口(含根路径)都出配置页,
     * config.html 的认证在 http_serve_html 内按文件名统一控制 */
    if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0 ||
        strcmp(req->uri, APP_WEB_CONFIG_PATH) == 0 ||
        strcmp(req->uri, APP_WEB_CONFIG_PATH "/") == 0 ||
        strcmp(req->uri, "/config.html") == 0) {
        file_name = "config.html";
    } else {
        return app_http_send_json_text(req, "404 Not Found",
                                   "{\"status\":\"error\",\"message\":\"not_found\"}");
    }
    return http_serve_html(req, file_name);
}

/* 强制门户落地页:免认证 200。CNA 弹窗先打开本页(探测 302 与 DHCP 选项 114 均指向
 * 这里),再自动(meta refresh)/手动按钮进入 /config,由系统弹 Basic 登录。
 * 纯静态标记,不读配置不取锁;所有 /api/ 接口认证不变 */
static esp_err_t portal_landing_handler(httpd_req_t *req)
{
    static const char portal_html[] =
        "<!doctype html><html lang=\"zh-CN\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\">"
        "<meta http-equiv=\"refresh\" content=\"1;url=" APP_WEB_CONFIG_PATH "\">"
        "<title>IotHub</title><style>"
        "body{font-family:sans-serif;margin:0;min-height:100vh;display:flex;align-items:center;justify-content:center;background:#f2f4f8;color:#1f2933}"
        ".card{background:#fff;border-radius:12px;padding:32px 28px;max-width:320px;text-align:center;box-shadow:0 4px 16px rgba(0,0,0,.08)}"
        "h2{margin:0 0 8px;font-size:20px}p{margin:0 0 4px;color:#6b7280;font-size:14px}"
        "a.btn{display:inline-block;margin-top:16px;padding:12px 24px;border-radius:8px;background:#2563eb;color:#fff;text-decoration:none;font-size:16px}"
        "</style></head><body><div class=\"card\">"
        "<h2>已连接到 IotHub</h2>"
        "<p>正在打开配置页…</p>"
        "<a class=\"btn\" href=\"" APP_WEB_CONFIG_PATH "\">打开配置页</a>"
        "</div></body></html>";

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    return app_http_ignore_client_disconnect(
        httpd_resp_send(req, portal_html, sizeof(portal_html) - 1), "portal page");
}

/* 修改 Web 登录凭据:验旧口令 → 可选改用户名 → 新盐+新口令哈希写入配置。
 * 成功后浏览器缓存的旧 Basic 凭据会 401,由页面提示重新登录。 */
static esp_err_t web_auth_put_handler(httpd_req_t *req)
{
    char old_password[65];
    char new_password[65];
    char new_username[33];
    uint8_t salt[16];
    esp_err_t save_err;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }

    if (!app_json_find_string(app_http_scratch_buf(), "old_password", old_password, sizeof(old_password)) ||
        strlen(old_password) == 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"old_password_required\"}");
    }
    if (!app_http_verify_password(old_password)) {
        return app_http_send_json_text(req, "403 Forbidden",
                                   "{\"status\":\"error\",\"message\":\"invalid_old_password\"}");
    }
    if (!app_json_find_string(app_http_scratch_buf(), "new_password", new_password, sizeof(new_password)) ||
        strlen(new_password) < 8) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"password_too_short\"}");
    }
    if (app_json_find_string(app_http_scratch_buf(), "new_username", new_username, sizeof(new_username)) &&
        strlen(new_username) > 0 &&
        strchr(new_username, ':') != NULL) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_username\"}");
    }

    esp_fill_random(salt, sizeof(salt));
    app_config_lock();
    if (app_json_find_string(app_http_scratch_buf(), "new_username", new_username, sizeof(new_username)) &&
        strlen(new_username) > 0) {
        app_copy_string(s_config.web_username, sizeof(s_config.web_username), new_username);
    }
    memcpy(s_config.web_pass_salt, salt, sizeof(salt));
    app_web_hash_password(salt, sizeof(salt), new_password, s_config.web_pass_hash);
    save_err = app_config_save(&s_config);
    app_config_unlock();

    if (save_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    ESP_LOGI(TAG, "Web credentials updated via config page");
    return app_http_send_json_text(req, NULL,
                               "{\"status\":\"saved\",\"message\":\"web credentials updated, please re-login\"}");
}

void app_http_start_webserver(void)
{
    static bool s_started = false;
    if (s_started) {
        return;
    }
    s_started = true;

    esp_log_level_set("httpd_uri", ESP_LOG_ERROR);

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = APP_HTTP_MAX_HANDLERS;
    /* httpd 起不来等于无管理面,保留 abort 重启重试;路由注册失败只降级对应功能组,
     * 不再 abort 整机(无人值守设备缺一组 API 好过无限重启) */
    ESP_ERROR_CHECK(httpd_start(&s_web.server, &config));
    {
        size_t index;
        const app_http_route_t core_routes[] = {
            {.uri = APP_WEB_PORTAL_PATH,     .method = HTTP_GET, .handler = portal_landing_handler},
            {.uri = "/api/v1/device/info",   .method = HTTP_GET, .handler = device_info_get_handler},
            {.uri = "/api/v1/device/status", .method = HTTP_GET, .handler = device_status_get_handler},
            {.uri = "/api/v1/config/web",    .method = HTTP_PUT, .handler = web_auth_put_handler},
        };
        for (index = 0; index < sizeof(core_routes) / sizeof(core_routes[0]); index++) {
            esp_err_t err = app_http_register_uri_handler(s_web.server,
                                                          core_routes[index].uri,
                                                          core_routes[index].method,
                                                          core_routes[index].handler);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "register %s failed: %s (route unavailable)",
                         core_routes[index].uri, esp_err_to_name(err));
            }
        }
    }

    {
        size_t index;
        static const struct {
            const char *name;
            esp_err_t (*register_fn)(httpd_handle_t);
        } route_groups[] = {
            {"network", http_network_register_routes},
            {"io",      http_io_register_routes},
            {"mqtt",    http_mqtt_register_routes},
            {"uart",    http_uart_register_routes},
            {"ota",     http_ota_register_routes},
        };
        for (index = 0; index < sizeof(route_groups) / sizeof(route_groups[0]); index++) {
            esp_err_t err = route_groups[index].register_fn(s_web.server);
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "register %s routes failed: %s (group unavailable)",
                         route_groups[index].name, esp_err_to_name(err));
            }
        }
    }

    {
        esp_err_t err = app_http_register_uri_handler(s_web.server,
                                                      "/*",
                                                      HTTP_GET,
                                                      index_handler);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "register catch-all route failed: %s (web pages unavailable)",
                     esp_err_to_name(err));
        }
    }
}
