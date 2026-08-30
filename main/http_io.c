#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "common.h"
#include "config.h"
#include "http_priv.h"
#include "http_utils.h"
#include "state.h"

static esp_err_t gpio_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t rv;
        uint8_t relay_active_high, relay_on, led_active_high, led_on, input_active_low;
        app_config_lock();
        relay_active_high = s_config.relay_active_high;
        relay_on = s_config.relay_on;
        led_active_high = s_config.led_active_high;
        led_on = s_config.led_on;
        input_active_low = s_config.input_active_low;
        app_config_unlock();
        rv = app_http_send_jsonf(req, NULL,
                               "{\"relay_gpio\":%ld,\"relay_active_high\":%s,\"relay_on\":%s,"
                               "\"led_gpio\":%ld,\"led_active_high\":%s,\"led_on\":%s,"
                               "\"input_gpio\":%ld,\"input_active_low\":%s,\"input_active\":%s,"
                               "\"uart_rx_gpio\":%ld,\"uart_tx_gpio\":%ld}",
                               (long)app_relay_gpio(),
                               relay_active_high ? "true" : "false",
                               relay_on ? "true" : "false",
                               (long)app_led_gpio(),
                               led_active_high ? "true" : "false",
                               led_on ? "true" : "false",
                               (long)app_input_gpio(),
                               input_active_low ? "true" : "false",
                               app_get_input_state() ? "true" : "false",
                               (long)app_uart_rx_gpio(),
                               (long)app_uart_tx_gpio());
        return rv;
    }
}

static esp_err_t gpio_put_handler(httpd_req_t *req)
{
    bool value;
    esp_err_t save_err;

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
    if (app_json_find_bool(app_http_scratch_buf(), "relay_on", &value)) {
        s_config.relay_on = value ? 1 : 0;
        app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
    }
    if (app_json_find_bool(app_http_scratch_buf(), "led_on", &value)) {
        s_config.led_on = value ? 1 : 0;
        app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
    }

    save_err = app_config_save(&s_config);
    app_config_unlock();

    if (save_err != ESP_OK) {
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
