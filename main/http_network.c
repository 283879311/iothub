#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "bt.h"
#include "cJSON.h"
#include "common.h"
#include "config.h"
#include "constants.h"
#include "http_priv.h"
#include "http_utils.h"
#include "identity.h"
#include "status.h"
#include "wifi.h"

const char *HTTP_TAG = "iothub";

static esp_err_t network_get_handler(httpd_req_t *req)
{
    char ip[16];
    char gateway[16];
    char netmask[16];
    cJSON *root;
    cJSON *profiles_json_root;
    char *serialized;
    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX] = {0};
    size_t profile_count = 0;
    esp_err_t err;
    uint8_t net_mode;
    char ap_ssid[sizeof(s_config.ap_ssid)];
    char ap_password[sizeof(s_config.ap_password)];
    char sta_ssid[sizeof(s_config.sta_ssid)];
    char sta_password[sizeof(s_config.sta_password)];
    bool sta_connected, sta_has_ip, startup_connect_active, startup_fallback_to_ap;
    char sta_ip[16];
    char sta_gateway[16];
    char sta_netmask[16];
    char last_disconnect[32];
    char startup_fallback_reason[32];
    int64_t startup_connect_deadline_ms;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    app_copy_string(ip, sizeof(ip), "192.168.8.1");
    app_copy_string(gateway, sizeof(gateway), "192.168.8.1");
    app_copy_string(netmask, sizeof(netmask), "255.255.255.0");
    if (app_config_load_wifi_profiles(profiles, APP_WIFI_PROFILE_MAX, &profile_count) != ESP_OK) {
        profile_count = 0;
    }

    app_config_lock();
    net_mode = s_config.net_mode;
    memcpy(ap_ssid, s_config.ap_ssid, sizeof(ap_ssid));
    memcpy(ap_password, s_config.ap_password, sizeof(ap_password));
    memcpy(sta_ssid, s_config.sta_ssid, sizeof(sta_ssid));
    memcpy(sta_password, s_config.sta_password, sizeof(sta_password));
    app_config_unlock();

    app_wifi_runtime_lock();
    sta_connected = s_wifi.sta_connected;
    sta_has_ip = s_wifi.sta_has_ip;
    startup_connect_active = s_wifi.startup_connect_active;
    startup_connect_deadline_ms = s_wifi.startup_connect_deadline_ms;
    startup_fallback_to_ap = s_wifi.startup_fallback_to_ap;
    memcpy(sta_ip, s_wifi.sta_ip, sizeof(sta_ip));
    memcpy(sta_gateway, s_wifi.sta_gateway, sizeof(sta_gateway));
    memcpy(sta_netmask, s_wifi.sta_netmask, sizeof(sta_netmask));
    memcpy(last_disconnect, s_wifi.last_disconnect, sizeof(last_disconnect));
    memcpy(startup_fallback_reason, s_wifi.startup_fallback_reason, sizeof(startup_fallback_reason));
    app_wifi_runtime_unlock();

    root = cJSON_CreateObject();
    if (root == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    cJSON_AddStringToObject(root, "mode", app_network_mode_to_string(net_mode));
    cJSON_AddStringToObject(root, "ap_ssid", ap_ssid);
    cJSON_AddStringToObject(root, "ap_password", ap_password);
    cJSON_AddStringToObject(root, "ap_ip", ip);
    cJSON_AddStringToObject(root, "gateway", gateway);
    cJSON_AddStringToObject(root, "netmask", netmask);
    cJSON_AddBoolToObject(root, "password_enabled", strlen(ap_password) > 0);
    cJSON_AddStringToObject(root, "sta_ssid", sta_ssid);
    cJSON_AddStringToObject(root, "sta_password", sta_password);
    cJSON_AddBoolToObject(root, "sta_connected", sta_connected);
    cJSON_AddBoolToObject(root, "sta_has_ip", sta_has_ip);
    cJSON_AddStringToObject(root, "sta_ip", sta_ip);
    cJSON_AddStringToObject(root, "sta_gateway", sta_gateway);
    cJSON_AddStringToObject(root, "sta_netmask", sta_netmask);
    cJSON_AddStringToObject(root, "last_disconnect", last_disconnect);
    cJSON_AddBoolToObject(root, "startup_connect_active", startup_connect_active);
    cJSON_AddNumberToObject(root, "startup_connect_deadline_ms", (double)startup_connect_deadline_ms);
    cJSON_AddBoolToObject(root, "startup_fallback_to_ap", startup_fallback_to_ap);
    cJSON_AddStringToObject(root, "startup_fallback_reason", startup_fallback_reason);

    profiles_json_root = cJSON_AddArrayToObject(root, "saved_sta_profiles");
    if (profiles_json_root != NULL) {
        size_t i;
        for (i = 0; i < profile_count; i++) {
            cJSON *profile = cJSON_CreateObject();
            if (profile == NULL) {
                continue;
            }
            cJSON_AddStringToObject(profile, "ssid", profiles[i].ssid);
            cJSON_AddStringToObject(profile, "password", profiles[i].password);
            cJSON_AddItemToArray(profiles_json_root, profile);
        }
    }

    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    err = app_http_send_json_text(req, NULL, serialized);
    cJSON_free(serialized);
    return err;
}

static esp_err_t network_put_handler(httpd_req_t *req)
{
    char value[65];
    bool ap_enabled_before;
    char current_ap_ip[16];
    char current_ap_gw[16];
    char current_ap_mask[16];
    esp_err_t save_err;
    esp_err_t profile_err = ESP_OK;
    bool sta_ssid_set = false;
    uint8_t net_mode_snapshot;
    char sta_ssid_snapshot[sizeof(s_config.sta_ssid)];

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    app_status_get_ap_network_strings(current_ap_ip, sizeof(current_ap_ip),
                                      current_ap_gw, sizeof(current_ap_gw),
                                      current_ap_mask, sizeof(current_ap_mask));

    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }

    app_config_lock();
    ap_enabled_before = app_network_mode_has_ap(s_config.net_mode);
    if (app_json_find_string(app_http_scratch_buf(), "ap_ssid", value, sizeof(s_config.ap_ssid)) && strlen(value) > 0) {
        app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), value);
    }
    if (app_json_find_string(app_http_scratch_buf(), "mode", value, sizeof(value))) {
        s_config.net_mode = app_network_mode_from_string(value);
    }
    if (app_json_find_string(app_http_scratch_buf(), "ap_password", value, sizeof(s_config.ap_password))) {
        if (strlen(value) > 0 && strlen(value) < 8) {
            app_config_unlock();
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"password_too_short\"}");
        }
        app_copy_string(s_config.ap_password, sizeof(s_config.ap_password), value);
    }
    if (app_json_find_string(app_http_scratch_buf(), "sta_ssid", value, sizeof(s_config.sta_ssid))) {
        app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), value);
        sta_ssid_set = true;
    }
    if (app_json_find_string(app_http_scratch_buf(), "sta_password", value, sizeof(s_config.sta_password))) {
        if (strlen(value) > 0 && strlen(value) < 8) {
            app_config_unlock();
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"sta_password_too_short\"}");
        }
        app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), value);
    }

    save_err = app_config_save(&s_config);
    if (save_err == ESP_OK && sta_ssid_set && strlen(s_config.sta_ssid) > 0) {
        profile_err = app_config_save_wifi_profile(s_config.sta_ssid, s_config.sta_password);
    }
    net_mode_snapshot = s_config.net_mode;
    memcpy(sta_ssid_snapshot, s_config.sta_ssid, sizeof(sta_ssid_snapshot));
    app_config_unlock();

    if (save_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    if (profile_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"wifi_profile_save_failed\"}");
    }
    app_wifi_sta_config_updated();
    return app_http_send_jsonf(req, NULL,
                           "{\"status\":\"saved\",\"message\":\"network config saved\","
                           "\"restart_required\":true,\"ap_enabled\":%s,"
                           "\"next_access_ip\":\"%s\",\"current_ap_ip\":\"%s\","
                           "\"mode\":\"%s\",\"sta_ssid\":\"%s\"}",
                           app_network_mode_has_ap(net_mode_snapshot) ? "true" : "false",
                           app_network_mode_has_ap(net_mode_snapshot) ? "192.168.8.1" : "",
                           ap_enabled_before ? current_ap_ip : "",
                           app_network_mode_to_string(net_mode_snapshot),
                           sta_ssid_snapshot);
}

static esp_err_t network_profile_delete_handler(httpd_req_t *req)
{
    char ssid[33];
    esp_err_t err;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t body_err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (body_err != ESP_OK) {
            return app_http_body_read_finished(body_err) ? ESP_OK : ESP_FAIL;
        }
    }
    if (!app_json_find_string(app_http_scratch_buf(), "ssid", ssid, sizeof(ssid)) || strlen(ssid) == 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"ssid_required\"}");
    }
    err = app_config_delete_wifi_profile(ssid);
    if (err == ESP_ERR_NOT_FOUND) {
        return app_http_send_json_text(req, "404 Not Found",
                                   "{\"status\":\"error\",\"message\":\"wifi_profile_not_found\"}");
    }
    if (err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"wifi_profile_delete_failed\"}");
    }
    return app_http_send_json_text(req, NULL,
                               "{\"status\":\"deleted\",\"message\":\"wifi profile deleted\"}");
}

static esp_err_t network_restart_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    app_http_send_json_text(req, NULL,
                        "{\"status\":\"restarting\",\"message\":\"network restart started\"}");
    if (!app_wifi_schedule_restart()) {
        ESP_LOGE(HTTP_TAG, "Failed to schedule Wi-Fi restart");
    }
    return ESP_OK;
}

static esp_err_t network_scan_handler(httpd_req_t *req)
{
    cJSON *root;
    cJSON *aps_array;
    uint16_t index;
    char *serialized;
    esp_err_t err;
    uint16_t scan_count;

    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }

    if (app_wifi_perform_scan() != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"wifi_scan_failed\"}");
    }

    app_wifi_runtime_lock();
    scan_count = s_wifi.scan_count;
    app_wifi_runtime_unlock();

    root = cJSON_CreateObject();
    if (root == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    cJSON_AddStringToObject(root, "status", "ok");
    cJSON_AddNumberToObject(root, "count", (double)scan_count);
    aps_array = cJSON_AddArrayToObject(root, "aps");
    if (aps_array != NULL) {
        for (index = 0; index < scan_count; index++) {
            cJSON *ap = cJSON_CreateObject();
            const char *ssid = "";
            int8_t rssi = 0;
            uint8_t authmode = 0;
            if (ap == NULL) {
                continue;
            }
            if (!app_wifi_scan_record_at(index, &ssid, &rssi, &authmode)) {
                cJSON_Delete(ap);
                continue;
            }
            cJSON_AddStringToObject(ap, "ssid", ssid);
            cJSON_AddNumberToObject(ap, "rssi", (double)rssi);
            cJSON_AddNumberToObject(ap, "auth", (double)authmode);
            cJSON_AddItemToArray(aps_array, ap);
        }
    }

    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    err = app_http_send_json_text(req, NULL, serialized);
    cJSON_free(serialized);
    return err;
}

static esp_err_t bluetooth_get_handler(httpd_req_t *req)
{
    if (!app_http_require_auth(req)) {
        return ESP_OK;
    }
    {
        esp_err_t rv;
        uint8_t bt_mode;
        char bt_device_name[sizeof(s_config.bt_device_name)];
        app_config_lock();
        bt_mode = s_config.bt_mode;
        memcpy(bt_device_name, s_config.bt_device_name, sizeof(bt_device_name));
        app_config_unlock();
        rv = app_http_send_jsonf(req, NULL,
                               "{\"mode\":\"%s\",\"device_name\":\"%s\",\"runtime_mode\":\"%s\",\"last_error\":\"%s\"}",
                               app_bt_mode_to_string(bt_mode),
                               bt_device_name,
                               app_bt_mode_to_string(s_bt_runtime_mode),
                               s_bt_last_error);
        return rv;
    }
}

static esp_err_t bluetooth_put_handler(httpd_req_t *req)
{
    char value[64];
    uint8_t previous_mode;
    bool managed_name_before;
    bool has_device_name = false;
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
    previous_mode = s_config.bt_mode;
    managed_name_before = app_bt_device_name_is_managed(s_config.bt_device_name);
    if (app_json_find_string(app_http_scratch_buf(), "mode", value, sizeof(value))) {
        s_config.bt_mode = app_bt_mode_from_string(value);
    }
    if (app_json_find_string(app_http_scratch_buf(), "device_name", value, sizeof(s_config.bt_device_name)) &&
        strlen(value) > 0) {
        has_device_name = true;
        app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), value);
    }
    if ((has_device_name && app_bt_device_name_is_managed(s_config.bt_device_name)) ||
        (!has_device_name && managed_name_before) ||
        (managed_name_before && previous_mode != s_config.bt_mode)) {
        app_set_managed_bt_device_name(s_config.bt_mode);
    }
    save_err = app_config_save(&s_config);
    app_config_unlock();

    if (save_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    if (xTaskCreate(app_bt_restart_task, "bt_restart", 4096, NULL, 5, NULL) != pdPASS) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"bt_restart_failed\"}");
    }

    app_http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"bluetooth config saved\"}");
    return ESP_OK;
}

esp_err_t http_network_register_routes(httpd_handle_t server)
{
    static bool s_registered = false;
    if (s_registered) {
        return ESP_OK;
    }
    s_registered = true;
    const app_http_route_t routes[] = {
        {.uri = "/api/v1/config/network",          .method = HTTP_GET,    .handler = network_get_handler},
        {.uri = "/api/v1/config/network",          .method = HTTP_PUT,    .handler = network_put_handler},
        {.uri = "/api/v1/config/network/profile",  .method = HTTP_DELETE, .handler = network_profile_delete_handler},
        {.uri = "/api/v1/network/restart",         .method = HTTP_POST,   .handler = network_restart_handler},
        {.uri = "/api/v1/network/scan",            .method = HTTP_GET,    .handler = network_scan_handler},
        {.uri = "/api/v1/config/bluetooth",        .method = HTTP_GET,    .handler = bluetooth_get_handler},
        {.uri = "/api/v1/config/bluetooth",        .method = HTTP_PUT,    .handler = bluetooth_put_handler},
    };
    size_t i;
    for (i = 0; i < sizeof(routes) / sizeof(routes[0]); i++) {
        ESP_RETURN_ON_ERROR(app_http_register_uri_handler(server,
                                                          routes[i].uri,
                                                          routes[i].method,
                                                          routes[i].handler), HTTP_TAG,
                            "register %s %s failed",
                            routes[i].method == HTTP_GET ? "GET" :
                            routes[i].method == HTTP_PUT ? "PUT" :
                            routes[i].method == HTTP_POST ? "POST" : "DELETE",
                            routes[i].uri);
    }
    return ESP_OK;
}
