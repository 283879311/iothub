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
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "common.h"
#include "config.h"
#include "constants.h"
#include "http.h"
#include "http_priv.h"
#include "http_utils.h"
#include "identity.h"
#include "mqtt.h"
#include "ota.h"
#include "sim.h"
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

static bool app_http_is_authorized(httpd_req_t *req)
{
    size_t header_len = httpd_req_get_hdr_value_len(req, "Authorization");
    char auth_header[80];

    if (header_len == 0 || header_len >= sizeof(auth_header)) {
        return false;
    }
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_header, sizeof(auth_header)) != ESP_OK) {
        return false;
    }
    return strcmp(auth_header, APP_WEB_AUTH_BASIC) == 0;
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

esp_err_t http_serve_html(httpd_req_t *req, const char *file_name)
{
    char path[96];
    FILE *file;
    char chunk[256];
    size_t read_bytes = 0;

    snprintf(path, sizeof(path), "%s/%s", APP_BASE_PATH, file_name);
    file = fopen(path, "rb");
    if (file == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"page_open_failed\"}");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
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

static esp_err_t index_handler(httpd_req_t *req)
{
    const char *file_name = NULL;
    bool requires_auth = false;

    if (strcmp(req->uri, "/") == 0 || strcmp(req->uri, "/index.html") == 0) {
        file_name = "index.html";
    } else if (strcmp(req->uri, APP_WEB_CONFIG_PATH) == 0 ||
               strcmp(req->uri, APP_WEB_CONFIG_PATH "/") == 0 ||
               strcmp(req->uri, "/config.html") == 0) {
        file_name = "config.html";
        requires_auth = true;
    } else {
        return app_http_send_json_text(req, "404 Not Found",
                                   "{\"status\":\"error\",\"message\":\"not_found\"}");
    }
    if (requires_auth && !app_http_require_auth(req)) {
        return ESP_OK;
    }
    return http_serve_html(req, file_name);
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
    ESP_ERROR_CHECK(httpd_start(&s_web.server, &config));
    {
        size_t index;
        const app_http_route_t core_routes[] = {
            {.uri = "/api/v1/device/info",   .method = HTTP_GET, .handler = device_info_get_handler},
            {.uri = "/api/v1/device/status", .method = HTTP_GET, .handler = device_status_get_handler},
        };
        for (index = 0; index < sizeof(core_routes) / sizeof(core_routes[0]); index++) {
            ESP_ERROR_CHECK(app_http_register_uri_handler(s_web.server,
                                                          core_routes[index].uri,
                                                          core_routes[index].method,
                                                          core_routes[index].handler));
        }
    }

    ESP_ERROR_CHECK(http_network_register_routes(s_web.server));
    ESP_ERROR_CHECK(http_io_register_routes(s_web.server));
    ESP_ERROR_CHECK(http_mqtt_register_routes(s_web.server));
    ESP_ERROR_CHECK(http_uart_register_routes(s_web.server));
    ESP_ERROR_CHECK(http_ota_register_routes(s_web.server));

    ESP_ERROR_CHECK(app_http_register_uri_handler(s_web.server,
                                                  "/*",
                                                  HTTP_GET,
                                                  index_handler));
}
