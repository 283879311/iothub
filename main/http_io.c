#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "cmd.h"
#include "common.h"
#include "config.h"
#include "config_apply.h"
#include "http_priv.h"
#include "http_utils.h"
#include "ota.h"
#include "state.h"

static esp_err_t gpio_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t rv;
        uint8_t relay_active_high, led_active_high, input_active_low;
        uint8_t relay_policy, led_policy;
        app_io_state_t io;

        app_config_lock();
        relay_active_high = s_config.relay_active_high;
        led_active_high = s_config.led_active_high;
        input_active_low = s_config.input_active_low;
        relay_policy = s_config.relay_power_policy;
        led_policy = s_config.led_power_policy;
        app_config_unlock();
        app_io_get_state(&io);
        rv = app_http_send_jsonf(req, NULL,
                               "{\"relay_gpio\":%ld,\"relay_active_high\":%s,\"relay_on\":%s,"
                               "\"relay_power_policy\":%u,"
                               "\"led_gpio\":%ld,\"led_active_high\":%s,\"led_on\":%s,"
                               "\"led_power_policy\":%u,"
                               "\"input_gpio\":%ld,\"input_active_low\":%s,\"input_active\":%s,"
                               "\"io_seq\":%u,\"io_source\":\"%s\","
                               "\"uart_rx_gpio\":%ld,\"uart_tx_gpio\":%ld}",
                               (long)app_relay_gpio(),
                               relay_active_high ? "true" : "false",
                               io.relay_on ? "true" : "false",
                               (unsigned)relay_policy,
                               (long)app_led_gpio(),
                               led_active_high ? "true" : "false",
                               io.led_on ? "true" : "false",
                               (unsigned)led_policy,
                               (long)app_input_gpio(),
                               input_active_low ? "true" : "false",
                               io.input_active ? "true" : "false",
                               (unsigned)io.seq, app_io_source_name(io.last_source),
                               (long)app_uart_rx_gpio(),
                               (long)app_uart_tx_gpio());
        return rv;
    }
}

static esp_err_t command_post_handler(httpd_req_t *req)
{
    cJSON *root = NULL;
    cJSON *resp = NULL;
    cJSON *envelope = NULL;
    const cJSON *id_item;
    app_cmd_result_t out = {0};
    const char *err = NULL;
    esp_err_t result;
    int id = 0;
    bool has_id = false;
    char *body;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t read_err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (read_err != ESP_OK) {
            return app_http_body_read_finished(read_err) ? ESP_OK : ESP_FAIL;
        }
    }
    root = cJSON_Parse(app_http_scratch_buf());
    if (!cJSON_IsObject(root)) {
        cJSON_Delete(root);
        return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"ok\":false,\"error\":\"invalid_json\"}");
    }
    id_item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsNumber(id_item)) {
        id = (int)id_item->valuedouble;
        has_id = true;
    }
    result = app_cmd_execute_ex(root, APP_CMD_SOURCE_HTTP, &resp, &out, &err);
    envelope = cJSON_CreateObject();
    if (envelope == NULL) {
        cJSON_Delete(resp);
        cJSON_Delete(root);
        return app_http_send_json_text(req, "500 Internal Server Error",
                                       "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }
    if (has_id) {
        cJSON_AddNumberToObject(envelope, "id", (double)id);
    }
    if (result != ESP_OK || resp == NULL) {
        cJSON_AddBoolToObject(envelope, "ok", false);
        cJSON_AddStringToObject(envelope, "error", err != NULL ? err : "internal_error");
        cJSON_Delete(resp);
    } else {
        cJSON_AddBoolToObject(envelope, "ok", true);
        cJSON_AddItemToObject(envelope, "result", resp);
        resp = NULL;
    }
    body = cJSON_PrintUnformatted(envelope);
    cJSON_Delete(envelope);
    cJSON_Delete(root);
    if (body == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                       "{\"ok\":false,\"error\":\"out_of_memory\"}");
    }
    result = app_http_send_json_text(req, NULL, body);
    cJSON_free(body);
    if (out.reboot_after_response) {
        app_reboot_schedule("http_cmd_reboot");
    }
    return result;
}

static esp_err_t gpio_put_handler(httpd_req_t *req)
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

    /* 配置面只处理策略/极性;GPIO 开关状态只走 /api/v1/command。 */
    err = config_apply_json(app_http_scratch_buf(), &changed, &effect);
    cJSON_Delete(changed);

    if (err == ESP_ERR_INVALID_ARG || err == ESP_ERR_NOT_SUPPORTED) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_config\"}");
    }
    if (err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    return gpio_get_handler(req);
}

esp_err_t http_io_register_routes(httpd_handle_t server)
{
    static bool s_registered = false;
    if (s_registered) {
        return ESP_OK;
    }
    s_registered = true;
    const app_http_route_t routes[] = {
        {.uri = "/api/v1/config/gpio", .method = HTTP_GET, .handler = gpio_get_handler},
        {.uri = "/api/v1/config/gpio", .method = HTTP_PUT, .handler = gpio_put_handler},
        {.uri = "/api/v1/command",     .method = HTTP_POST, .handler = command_post_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(app_http_register_uri_handler(server,
                                                          routes[i].uri,
                                                          routes[i].method,
                                                          routes[i].handler), HTTP_TAG,
                            "register gpio route failed");
    }
    return ESP_OK;
}
