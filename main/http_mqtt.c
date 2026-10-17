#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "cJSON.h"

#include "common.h"
#include "config.h"
#include "config_apply.h"
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
                               "{\"backend\":\"%s\",\"host\":\"%s\",\"port\":%u,"
                               "\"token\":\"\",\"token_set\":%s,"
                               "\"use_tls\":%s,\"connected\":%s,\"last_error\":\"%s\",\"publish_count\":%u}",
                               mqtt_backend, mqtt_host, mqtt_port,
                               mqtt_token[0] != '\0' ? "true" : "false",
                               mqtt_use_tls ? "true" : "false",
                               mqtt_connected ? "true" : "false",
                               last_error, publish_count);
        return rv;
    }
}

static esp_err_t mqtt_put_handler(httpd_req_t *req)
{
    cfg_effect_t effect = CFG_EFFECT_NONE;
    cJSON *changed = NULL;
    esp_err_t err;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    {
        esp_err_t read_err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (read_err != ESP_OK) {
            return app_http_body_read_finished(read_err) ? ESP_OK : ESP_FAIL;
        }
    }

    /* 配置写入统一走 config_apply(唯一写入点)。MQTT 域的生效方式是"子系统重启"
     * (= 异步重连),不是整机重启;token 留空=保持、token_clear=清除 的既有语义
     * 由字段表的 empty_keeps / 伪字段承担。 */
    err = config_apply_json(app_http_scratch_buf(), &changed, &effect);
    {
        bool had_changes = (changed != NULL && cJSON_GetArraySize(changed) > 0);

        cJSON_Delete(changed);

        /* 1.3-43:用户保存 MQTT 配置 = 明确要(重新)连接。即使字段与已存值
         * 相同(config_apply 不触发域生效),也要清致命位并调度一次重连 ——
         * 否则"broker 认证拒绝后,改好 token 保存同值配置"会毫无反应
         * (实测:保存相同配置时既不重连也不报错)。 */
        if (err == ESP_OK) {
            app_mqtt_clear_fatal();
            if (!had_changes) {
                (void)app_mqtt_schedule_restart();
            }
        }
    }

    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_SUPPORTED) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_config\"}");
    }
    if (err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    app_http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"mqtt config saved, connection will restart\"}");
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
