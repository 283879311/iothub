#include <stdbool.h>
#include <stddef.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"

#include "http_priv.h"
#include "ota.h"

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_ota_handle_firmware_upload(req, app_http_scratch_buf(), app_http_scratch_size());
}

static esp_err_t storage_upload_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    return app_ota_handle_storage_upload(req, app_http_scratch_buf(), app_http_scratch_size());
}

esp_err_t http_ota_register_routes(httpd_handle_t server)
{
    static bool s_registered = false;
    if (s_registered) {
        return ESP_OK;
    }
    s_registered = true;
    const app_http_route_t routes[] = {
        {.uri = "/api/v1/ota/upload",     .method = HTTP_POST, .handler = ota_upload_handler},
        {.uri = "/api/v1/storage/upload", .method = HTTP_POST, .handler = storage_upload_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(app_http_register_uri_handler(server,
                                                          routes[i].uri,
                                                          routes[i].method,
                                                          routes[i].handler), HTTP_TAG,
                            "register ota/storage route failed");
    }
    return ESP_OK;
}
