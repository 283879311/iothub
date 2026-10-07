#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "config.h"
#include "http_priv.h"
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
    if (!app_http_require_auth(req)) {
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
