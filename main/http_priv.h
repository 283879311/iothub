#ifndef IOTHUB_HTTP_PRIV_H
#define IOTHUB_HTTP_PRIV_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_http_server.h"

#include "config.h"

#ifdef __cplusplus
extern "C" {
#endif

extern app_config_t s_config;
extern const char *HTTP_TAG;

char *app_http_scratch_buf(void);
size_t app_http_scratch_size(void);

esp_err_t app_http_ignore_client_disconnect(esp_err_t err, const char *context);
bool      app_http_body_read_finished(esp_err_t err);
esp_err_t app_http_send_auth_challenge(httpd_req_t *req);
bool      app_http_require_auth(httpd_req_t *req);
esp_err_t http_read_body(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t http_serve_html(httpd_req_t *req, const char *file_name);

typedef struct {
    const char *uri;
    httpd_method_t method;
    esp_err_t (*handler)(httpd_req_t *req);
} app_http_route_t;

esp_err_t http_network_register_routes(httpd_handle_t server);
esp_err_t http_io_register_routes(httpd_handle_t server);
esp_err_t http_mqtt_register_routes(httpd_handle_t server);
esp_err_t http_uart_register_routes(httpd_handle_t server);
esp_err_t http_ota_register_routes(httpd_handle_t server);

esp_err_t app_http_register_uri_handler(httpd_handle_t server,
                                        const char *uri,
                                        httpd_method_t method,
                                        esp_err_t (*handler)(httpd_req_t *req));

#ifdef __cplusplus
}
#endif

#endif
