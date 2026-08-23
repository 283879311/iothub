#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "common.h"
#include "config.h"
#include "http_priv.h"
#include "http_utils.h"
#include "mqtt.h"

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_http_send_jsonf(req, NULL,
                           "{\"backend\":\"%s\",\"host\":\"%s\",\"port\":%u,\"token\":\"%s\","
                           "\"use_tls\":%s,\"connected\":%s,\"last_error\":\"%s\",\"publish_count\":%u}",
                           s_config.mqtt_backend,
                           s_config.mqtt_host,
                           s_config.mqtt_port,
                           s_config.mqtt_token,
                           s_config.mqtt_use_tls ? "true" : "false",
                           s_mqtt.connected ? "true" : "false",
                           s_mqtt.last_error,
                           s_mqtt.publish_count);
}

static esp_err_t mqtt_put_handler(httpd_req_t *req)
{
    char value[128];
    uint16_t port;
    bool bool_value;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }

    if (app_json_find_string(app_http_scratch_buf(), "backend", value, sizeof(s_config.mqtt_backend)) && strlen(value) > 0) {
        app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), value);
    }
    if (app_json_find_string(app_http_scratch_buf(), "host", value, sizeof(s_config.mqtt_host)) && strlen(value) > 0) {
        app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), value);
    }
    if (app_json_find_string(app_http_scratch_buf(), "token", value, sizeof(s_config.mqtt_token))) {
        app_copy_string(s_config.mqtt_token, sizeof(s_config.mqtt_token), value);
    }
    if (app_json_find_u16(app_http_scratch_buf(), "port", &port) && port > 0) {
        s_config.mqtt_port = port;
    }
    if (app_json_find_bool(app_http_scratch_buf(), "use_tls", &bool_value)) {
        s_config.mqtt_use_tls = bool_value;
    }

    if (app_config_save(&s_config) != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    app_http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"mqtt config saved, connection will restart\"}");
    xTaskCreate(app_mqtt_restart_task, "mqtt_restart", 4096, NULL, 5, NULL);
    return ESP_OK;
}

esp_err_t http_mqtt_register_routes(httpd_handle_t server)
{
    static bool s_registered = false;
    if (s_registered) {
        return ESP_OK;
    }
    s_registered = true;
    const app_http_route_t routes[] = {
        {.uri = "/api/v1/config/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler},
        {.uri = "/api/v1/config/mqtt", .method = HTTP_PUT, .handler = mqtt_put_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(app_http_register_uri_handler(server,
                                                          routes[i].uri,
                                                          routes[i].method,
                                                          routes[i].handler), HTTP_TAG,
                            "register mqtt route failed");
    }
    return ESP_OK;
}
