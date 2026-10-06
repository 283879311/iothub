#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_priv.h"
#include "sim.h"

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

esp_err_t http_sim_register_routes(httpd_handle_t server)
{
    static bool s_registered = false;
    if (s_registered) {
        return ESP_OK;
    }
    s_registered = true;
    const app_http_route_t routes[] = {
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
                            "register sim route failed: %s", routes[i].uri);
    }
    return ESP_OK;
}
