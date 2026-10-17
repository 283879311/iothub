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
/* 当前已建立的 HTTP 连接数(观测用,见 1.3版本优化.md 1.3-19) */
size_t app_http_client_count(void);
/* 配置页是否正在流式发送(MQTT 重连避让用,见 1.3-36) */
bool   app_http_serving_page(void);
/* HTTP 并发连接上限(见 1.3版本优化.md 1.3-28) */
size_t app_http_client_max(void);
bool      app_http_body_read_finished(esp_err_t err);
esp_err_t app_http_send_auth_challenge(httpd_req_t *req);
bool      app_http_require_auth(httpd_req_t *req);
/* 仅接受 Cookie 会话认证(拒绝 Basic),用于管理页/状态轮询,配合单连接会话接管 */
bool      app_http_require_auth_cookie(httpd_req_t *req);
/* 取请求对端 IPv4 地址(试连护栏等按客户端来源判定) */
bool      app_http_peer_ip(httpd_req_t *req, char *out, size_t out_size);
/* Web 登录凭据校验(门户表单 / BLE login 命令共用) */
bool      app_http_check_credentials(const char *username, const char *password);
esp_err_t http_read_body(httpd_req_t *req, char *buf, size_t buf_len);
esp_err_t http_serve_html(httpd_req_t *req, const char *file_name);
/* 页面句柄缓存失效(storage OTA 更新后调用) */
void      app_http_html_cache_reset(void);
/* 开机预热页面句柄(httpd 启动后调用) */
void      app_http_html_cache_warmup(void);

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
