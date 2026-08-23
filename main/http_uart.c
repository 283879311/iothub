#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "config.h"
#include "http_priv.h"
#include "sim.h"
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

static esp_err_t uart_runtime_get_handler(httpd_req_t *req)
{
    return app_uart_send_runtime_json(req, "uart_runtime");
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
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    return app_uart_handle_send_request(req, app_http_scratch_buf());
}

static esp_err_t uart_keepalive_start_handler(httpd_req_t *req)
{
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    return app_uart_handle_keepalive_start_request(req, app_http_scratch_buf());
}

static esp_err_t uart_keepalive_stop_handler(httpd_req_t *req)
{
    return app_uart_handle_keepalive_stop_request(req);
}

static esp_err_t sim_status_get_handler(httpd_req_t *req)
{
    return app_sim_send_status_json(req, "sim_status");
}

static esp_err_t sim_start_handler(httpd_req_t *req)
{
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    return app_sim_handle_start_request(req, app_http_scratch_buf());
}

static esp_err_t sim_exit_handler(httpd_req_t *req)
{
    return app_sim_handle_exit_request(req);
}

static esp_err_t sim_stop_handler(httpd_req_t *req)
{
    return app_sim_handle_stop_request(req);
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
        {.uri = "/api/v1/uart/runtime",             .method = HTTP_GET,  .handler = uart_runtime_get_handler},
        {.uri = "/api/v1/uart/console",             .method = HTTP_GET,  .handler = uart_console_get_handler},
        {.uri = "/api/v1/uart/console/clear",       .method = HTTP_POST, .handler = uart_console_clear_handler},
        {.uri = "/api/v1/uart/send",                .method = HTTP_POST, .handler = uart_send_handler},
        {.uri = "/api/v1/uart/keepalive/start",     .method = HTTP_POST, .handler = uart_keepalive_start_handler},
        {.uri = "/api/v1/uart/keepalive/stop",      .method = HTTP_POST, .handler = uart_keepalive_stop_handler},
        {.uri = "/api/v1/sim/status",               .method = HTTP_GET,  .handler = sim_status_get_handler},
        {.uri = "/api/v1/sim/start",                .method = HTTP_POST, .handler = sim_start_handler},
        {.uri = "/api/v1/sim/exit",                 .method = HTTP_POST, .handler = sim_exit_handler},
        {.uri = "/api/v1/sim/stop",                 .method = HTTP_POST, .handler = sim_stop_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(app_http_register_uri_handler(server,
                                                      routes[i].uri,
                                                      routes[i].method,
                                                      routes[i].handler), HTTP_TAG,
                            "register uart/sim route failed: %s", routes[i].uri);
    }
    return ESP_OK;
}
