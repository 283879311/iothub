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
    {
        esp_err_t rv;
        bool mqtt_connected;
        uint32_t publish_count;
        char mqtt_backend[sizeof(s_config.mqtt_backend)];
        char mqtt_host[sizeof(s_config.mqtt_host)];
        char mqtt_token[sizeof(s_config.mqtt_token)];
        uint16_t mqtt_port;
        uint8_t mqtt_use_tls;
        char last_error[sizeof(s_mqtt.last_error)];
        app_config_lock();
        memcpy(mqtt_backend, s_config.mqtt_backend, sizeof(mqtt_backend));
        memcpy(mqtt_host, s_config.mqtt_host, sizeof(mqtt_host));
        memcpy(mqtt_token, s_config.mqtt_token, sizeof(mqtt_token));
        mqtt_port = s_config.mqtt_port;
        mqtt_use_tls = s_config.mqtt_use_tls;
        app_config_unlock();
        app_mqtt_lock();
        mqtt_connected = s_mqtt.connected;
        publish_count = s_mqtt.publish_count;
        memcpy(last_error, s_mqtt.last_error, sizeof(last_error));
        app_mqtt_unlock();
        rv = app_http_send_jsonf(req, NULL,
                               "{\"backend\":\"%s\",\"host\":\"%s\",\"port\":%u,\"token\":\"%s\","
                               "\"use_tls\":%s,\"connected\":%s,\"last_error\":\"%s\",\"publish_count\":%u}",
                               mqtt_backend, mqtt_host, mqtt_port, mqtt_token,
                               mqtt_use_tls ? "true" : "false",
                               mqtt_connected ? "true" : "false",
                               last_error, publish_count);
        return rv;
    }
}

static esp_err_t mqtt_put_handler(httpd_req_t *req)
{
    char value[128];
    uint16_t port;
    bool bool_value;
    esp_err_t save_err;
    bool need_restart = false;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }

    app_config_lock();
    if (app_json_find_string(app_http_scratch_buf(), "backend", value, sizeof(s_config.mqtt_backend)) && strlen(value) > 0) {
        app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), value);
        need_restart = true;
    }
    if (app_json_find_string(app_http_scratch_buf(), "host", value, sizeof(s_config.mqtt_host)) && strlen(value) > 0) {
        app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), value);
        need_restart = true;
    }
    if (app_json_find_string(app_http_scratch_buf(), "token", value, sizeof(s_config.mqtt_token))) {
        app_copy_string(s_config.mqtt_token, sizeof(s_config.mqtt_token), value);
        need_restart = true;
    }
    if (app_json_find_u16(app_http_scratch_buf(), "port", &port) && port > 0) {
        if (s_config.mqtt_port != port) {
            s_config.mqtt_port = port;
            need_restart = true;
        }
    }
    if (app_json_find_bool(app_http_scratch_buf(), "use_tls", &bool_value)) {
        if (s_config.mqtt_use_tls != (uint8_t)bool_value) {
            s_config.mqtt_use_tls = (uint8_t)bool_value;
            need_restart = true;
        }
    }

    save_err = app_config_save(&s_config);
    app_config_unlock();

    if (save_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    app_http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"mqtt config saved, connection will restart\"}");
    if (need_restart && !app_mqtt_schedule_restart()) {
        ESP_LOGE(HTTP_TAG, "Failed to schedule MQTT restart after config save");
    }
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
