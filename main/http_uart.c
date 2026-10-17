#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "config.h"
#include "data_pipe.h"
#include "http_priv.h"
#include "http_utils.h"
#include "uart.h"

static esp_err_t uart_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_uart_send_runtime_json(req, "uart_runtime");
}

static esp_err_t uart_put_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    return app_uart_handle_config_request(req, app_http_scratch_buf());
}

static esp_err_t uart_console_get_handler(httpd_req_t *req)
{
    /* 轮询接口必须走 Cookie,否则 Basic 凭据会绕过单连接会话接管 */
    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }
    return app_http_ignore_client_disconnect(app_uart_send_console_json(req), "uart console");
}

static esp_err_t uart_console_clear_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_uart_handle_clear_console_request(req);
}

static esp_err_t uart_send_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    return app_uart_handle_send_request(req, app_http_scratch_buf());
}

/* 数据面回环开关(1.3版本优化.md 1.3-03 验收):POST {\"enabled\":true|false}
 * 运行期生效、不落 NVS;用于让"BLE<->BLE"自环,从而用一次手机写入验证收发双向。 */
static esp_err_t data_loopback_handler(httpd_req_t *req)
{
    bool on = false;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    if (!app_json_find_bool(app_http_scratch_buf(), "enabled", &on)) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"enabled_required\"}");
    }
    data_pipe_set_loopback(on);
    return app_http_send_jsonf(req, NULL, "{\"status\":\"ok\",\"loopback\":%s}",
                               on ? "true" : "false");
}

esp_err_t http_uart_register_routes(httpd_handle_t server)
{
    static bool s_registered = false;
    if (s_registered) {
        return ESP_OK;
    }
    s_registered = true;
    const app_http_route_t routes[] = {
        {.uri = "/api/v1/config/uart",              .method = HTTP_GET,  .handler = uart_get_handler},
        {.uri = "/api/v1/config/uart",              .method = HTTP_PUT,  .handler = uart_put_handler},
        {.uri = "/api/v1/uart/console",             .method = HTTP_GET,  .handler = uart_console_get_handler},
        {.uri = "/api/v1/uart/console/clear",       .method = HTTP_POST, .handler = uart_console_clear_handler},
        {.uri = "/api/v1/uart/send",                .method = HTTP_POST, .handler = uart_send_handler},
        {.uri = "/api/v1/data/loopback",            .method = HTTP_POST, .handler = data_loopback_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(app_http_register_uri_handler(server,
                                                      routes[i].uri,
                                                      routes[i].method,
                                                      routes[i].handler), HTTP_TAG,
                            "register uart route failed: %s", routes[i].uri);
    }
    return ESP_OK;
}
