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
#include "config_apply.h"
#include "constants.h"
#include "http_priv.h"
#include "http_utils.h"
#include "identity.h"
#include "ota.h"
#include "status.h"
#include "wifi.h"

const char *HTTP_TAG = "iothub";

static esp_err_t network_get_handler(httpd_req_t *req)
{
    char ip[16];
    char gateway[16];
    char netmask[16];
    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX] = {0};
    size_t profile_count = 0;
    esp_err_t rv;
    /* 1.3-12:变长 profiles 数组先在静态缓冲里拼装(受 app_json_build_lock() 保护),零堆 */
    static char profiles_buf[1024];
    uint8_t net_mode;
    char ap_ssid[sizeof(s_config.ap_ssid)];
    char ap_password[sizeof(s_config.ap_password)];
    char sta_ssid[sizeof(s_config.sta_ssid)];
    char sta_password[sizeof(s_config.sta_password)];
    bool sta_connected, sta_has_ip, startup_connect_active, startup_fallback_to_ap;
    bool sta_use_static;
    char sta_static_ip[sizeof(s_config.sta_static_ip)];
    char sta_static_gateway[sizeof(s_config.sta_static_gateway)];
    char sta_static_netmask[sizeof(s_config.sta_static_netmask)];
    char sta_static_dns1[sizeof(s_config.sta_static_dns1)];
    char sta_static_dns2[sizeof(s_config.sta_static_dns2)];
    char sta_ip[16];
    char sta_gateway[16];
    char sta_netmask[16];
    char last_disconnect[32];
    char startup_fallback_reason[32];
    int64_t startup_connect_deadline_ms;

    if (!app_http_require_auth_cookie(req)) {
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
    sta_use_static = s_config.sta_use_static;
    memcpy(sta_static_ip, s_config.sta_static_ip, sizeof(sta_static_ip));
    memcpy(sta_static_gateway, s_config.sta_static_gateway, sizeof(sta_static_gateway));
    memcpy(sta_static_netmask, s_config.sta_static_netmask, sizeof(sta_static_netmask));
    memcpy(sta_static_dns1, s_config.sta_static_dns1, sizeof(sta_static_dns1));
    memcpy(sta_static_dns2, s_config.sta_static_dns2, sizeof(sta_static_dns2));
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

    /* 1.3-12:零堆直出。字段名/顺序/格式与改造前**逐字节一致**(基线 /tmp/base_config_network.json)。
     * 注意:profiles 的拼装也必须在锁内 —— app_json_escaped() 的轮转槽是全局共享的。 */
    app_json_build_lock();
    {
        size_t used = 0;
        size_t i;

        profiles_buf[0] = '\0';
        for (i = 0; i < profile_count; i++) {
            int n = snprintf(profiles_buf + used, sizeof(profiles_buf) - used,
                             "%s{\"ssid\":\"%s\",\"password_set\":%s}",
                             (i == 0) ? "" : ",", app_json_escaped(profiles[i].ssid),
                             (strlen(profiles[i].password) > 0) ? "true" : "false");
            if (n < 0 || (size_t)n >= sizeof(profiles_buf) - used) {
                ESP_LOGW(HTTP_TAG, "saved wifi profiles truncated at index %u", (unsigned)i);
                break;
            }
            used += (size_t)n;
        }
    }
    rv = app_http_send_jsonf(req, NULL,
        "{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_password\":\"\",\"ap_ip\":\"%s\""
        ",\"gateway\":\"%s\",\"netmask\":\"%s\",\"password_enabled\":%s,\"sta_ssid\":\"%s\""
        ",\"sta_password\":\"\",\"sta_password_set\":%s,\"sta_connected\":%s"
        ",\"sta_has_ip\":%s,\"sta_ip\":\"%s\",\"sta_gateway\":\"%s\",\"sta_netmask\":\"%s\""
        ",\"last_disconnect\":\"%s\",\"startup_connect_active\":%s"
        ",\"startup_connect_deadline_ms\":%lld,\"startup_fallback_to_ap\":%s"
        ",\"startup_fallback_reason\":\"%s\",\"saved_sta_profiles\":[%s]"
        ",\"sta_use_static\":%s,\"sta_static_ip\":\"%s\",\"sta_static_gateway\":\"%s\""
        ",\"sta_static_netmask\":\"%s\",\"sta_static_dns1\":\"%s\",\"sta_static_dns2\":\"%s\"}",
        app_network_mode_to_string(net_mode),
        app_json_escaped(ap_ssid), ip, gateway, netmask,
        (strlen(ap_password) > 0) ? "true" : "false",
        app_json_escaped(sta_ssid),
        (strlen(sta_password) > 0) ? "true" : "false",
        sta_connected ? "true" : "false", sta_has_ip ? "true" : "false", sta_ip,
        sta_gateway, sta_netmask, app_json_escaped(last_disconnect),
        startup_connect_active ? "true" : "false", (long long)startup_connect_deadline_ms,
        startup_fallback_to_ap ? "true" : "false", app_json_escaped(startup_fallback_reason),
        profiles_buf,
        sta_use_static ? "true" : "false",
        app_json_escaped(sta_static_ip), app_json_escaped(sta_static_gateway),
        app_json_escaped(sta_static_netmask), app_json_escaped(sta_static_dns1),
        app_json_escaped(sta_static_dns2));
    app_json_build_unlock();
    return rv;
}

static esp_err_t network_put_handler(httpd_req_t *req)
{
    bool ap_enabled_before;
    char current_ap_ip[16];
    char current_ap_gw[16];
    char current_ap_mask[16];
    esp_err_t profile_err = ESP_OK;
    uint8_t net_mode_snapshot;
    char sta_ssid_snapshot[sizeof(s_config.sta_ssid)];
    char requested_mode[16];
    bool mode_adjusted = false;

    if (!app_http_require_auth_cookie(req)) {
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
    app_config_unlock();

    /* 统一写入层(1.3版本优化.md 1.3-09 · B3):校验/比较/落盘/生效全部交给 config_apply,
     * 本 handler 只承担"副作用 + 响应体"。既有语义完整保留:
     *   - 密码留空 = 保持(字段表 empty_keeps);ap/sta_password_clear = 清除(伪字段)
     *   - HTTP 短名 `mode` 是 `net_mode` 的别名
     *   - 未知字段忽略;非法值(含口令 < 8)整体拒绝并返回 400 invalid_config
     *   - 同值重复提交不落盘、不触发生效动作 */
    {
        cJSON *changed = NULL;
        cfg_effect_t effect = CFG_EFFECT_NONE;
        esp_err_t apply_err = config_apply_json(app_http_scratch_buf(), &changed, &effect);

        if (changed != NULL) {
            cJSON_Delete(changed);
        }
        if (apply_err == ESP_ERR_INVALID_ARG || apply_err == ESP_ERR_NOT_SUPPORTED) {
            char err_detail[128] = "invalid_config";
            char err_json[192];
            config_apply_json_validate(app_http_scratch_buf(), err_detail, sizeof(err_detail));
            snprintf(err_json, sizeof(err_json),
                     "{\"status\":\"error\",\"message\":\"%s\"}", err_detail);
            return app_http_send_json_text(req, "400 Bad Request", err_json);
        }
        if (apply_err != ESP_OK) {
            return app_http_send_json_text(req, "500 Internal Server Error",
                                       "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
        }
    }

    /* 副作用:body 里带了非空 sta_ssid 时,把该 SSID +(可能已更新的)口令固化成一个已保存 profile */
    {
        char ssid_in_body[sizeof(s_config.sta_ssid)];
        bool ssid_provided = app_json_find_string(app_http_scratch_buf(), "sta_ssid",
                                                  ssid_in_body, sizeof(ssid_in_body)) &&
                             strlen(ssid_in_body) > 0;

        app_config_lock();
        net_mode_snapshot = s_config.net_mode;
        memcpy(sta_ssid_snapshot, s_config.sta_ssid, sizeof(sta_ssid_snapshot));
        if (ssid_provided) {
            profile_err = app_config_save_wifi_profile(s_config.sta_ssid, s_config.sta_password);
        }
        app_config_unlock();
    }

    /* 统一层的一致性推断(1.3-04:已存 STA SSID 却处于纯 AP → 自动启用 STA)会改掉生效模式,
     * 这里把"请求值 vs 生效值"的差异回报给前端,便于界面明确告知用户 */
    if (app_json_find_string(app_http_scratch_buf(), "mode", requested_mode, sizeof(requested_mode))) {
        mode_adjusted = (app_network_mode_from_string(requested_mode) != net_mode_snapshot);
        if (mode_adjusted) {
            ESP_LOGW(HTTP_TAG, "network save: requested mode '%s' adjusted to '%s' (STA SSID present)",
                     requested_mode, app_network_mode_to_string(net_mode_snapshot));
        }
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
                           "\"mode_adjusted\":%s,\"mode\":\"%s\",\"sta_ssid\":\"%s\"}",
                           app_network_mode_has_ap(net_mode_snapshot) ? "true" : "false",
                           app_network_mode_has_ap(net_mode_snapshot) ? "192.168.8.1" : "",
                           ap_enabled_before ? current_ap_ip : "",
                           mode_adjusted ? "true" : "false",
                           app_network_mode_to_string(net_mode_snapshot),
                           sta_ssid_snapshot);
}

/* 前端"保存前当场校验"接口:不修改配置、不落盘,只跑统一层校验规则。
 * 与 PUT /api/v1/config/network 共享同一套字段/跨字段校验,错语直接回显。 */
static esp_err_t network_validate_handler(httpd_req_t *req)
{
    char err_detail[128] = "invalid_config";

    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    if (config_apply_json_validate(app_http_scratch_buf(), err_detail, sizeof(err_detail)) == ESP_OK) {
        return app_http_send_json_text(req, NULL,
                                   "{\"status\":\"ok\",\"message\":\"valid\"}");
    }
    {
        char err_json[192];
        snprintf(err_json, sizeof(err_json),
                 "{\"status\":\"error\",\"message\":\"%s\"}", err_detail);
        return app_http_send_json_text(req, "400 Bad Request", err_json);
    }
}

/* 试连验证(1.3-37):真连一次候选 SSID/口令验证口令正确性。
 * 护栏:试连会拆掉当前 STA 连接,只有 AP 侧客户端(192.168.8.x)才安全;
 * 局域网(ST A)访问一律 409,提示改连设备热点。阻塞最多 ~20s(扫描+15s 试连)。 */
static esp_err_t network_tryconnect_handler(httpd_req_t *req)
{
    char ssid[33];
    char password[65];
    char peer_ip[16];
    app_wifi_try_result_t result;

    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }

    if (!app_http_peer_ip(req, peer_ip, sizeof(peer_ip)) ||
        strncmp(peer_ip, "192.168.8.", 10) != 0) {
        return app_http_send_json_text(req, "409 Conflict",
            "{\"status\":\"error\",\"message\":\"tryconnect_requires_ap\"}");
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }
    if (!app_json_find_string(app_http_scratch_buf(), "sta_ssid", ssid, sizeof(ssid)) ||
        strlen(ssid) == 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"ssid_required\"}");
    }
    if (!app_json_find_string(app_http_scratch_buf(), "sta_password", password, sizeof(password))) {
        password[0] = '\0';
    }

    result = app_wifi_try_connect(ssid, password, 15000);
    switch (result) {
    case APP_WIFI_TRY_OK:
        return app_http_send_json_text(req, NULL,
                                   "{\"status\":\"ok\",\"message\":\"password_ok\"}");
    case APP_WIFI_TRY_WRONG_PASSWORD:
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"wrong_password\"}");
    case APP_WIFI_TRY_AP_NOT_FOUND:
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"ap_not_found\"}");
    case APP_WIFI_TRY_TIMEOUT:
        return app_http_send_json_text(req, "504 Gateway Timeout",
                                   "{\"status\":\"error\",\"message\":\"connect_timeout\"}");
    case APP_WIFI_TRY_BUSY:
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"wifi_busy\"}");
    case APP_WIFI_TRY_INVALID_ARG:
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"invalid_ssid\"}");
    default:
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"connect_failed\"}");
    }
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
    if (!app_http_require_auth_cookie(req)) {
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

    if (!app_http_require_auth_cookie(req)) {
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
            char bssid[18];

            cJSON_AddStringToObject(ap, "ssid", ssid);
            cJSON_AddNumberToObject(ap, "rssi", (double)rssi);
            cJSON_AddNumberToObject(ap, "auth", (double)authmode);
            /* 1.3-21:同名 AP 撞名(CMCC-xxxx)很常见,BSSID 用于与手机侧比对确认身份 */
            if (app_wifi_scan_record_bssid(index, bssid, sizeof(bssid))) {
                cJSON_AddStringToObject(ap, "bssid", bssid);
            }
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
    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }
    {
        esp_err_t rv;
        uint8_t bt_mode;
        uint8_t bt_runtime_mode;
        char bt_device_name[sizeof(s_config.bt_device_name)];
        char bt_last_error[64];
        app_config_lock();
        bt_mode = s_config.bt_role;
        memcpy(bt_device_name, s_config.bt_device_name, sizeof(bt_device_name));
        app_config_unlock();
        app_bt_get_status(&bt_runtime_mode, bt_last_error, sizeof(bt_last_error));
        rv = app_http_send_jsonf(req, NULL,
                               "{\"mode\":\"%s\",\"device_name\":\"%s\",\"runtime_mode\":\"%s\",\"last_error\":\"%s\"}",
                               app_bt_mode_to_string(bt_mode),
                               bt_device_name,
                               app_bt_mode_to_string(bt_runtime_mode),
                               bt_last_error);
        return rv;
    }
}

static esp_err_t bluetooth_put_handler(httpd_req_t *req)
{
    char value[64];
    bool managed_name_before;
    bool has_device_name = false;
    esp_err_t save_err;

    if (!app_http_require_auth_cookie(req)) {
        return ESP_OK;
    }
    {
        esp_err_t err = http_read_body(req, app_http_scratch_buf(), app_http_scratch_size());
        if (err != ESP_OK) {
            return app_http_body_read_finished(err) ? ESP_OK : ESP_FAIL;
        }
    }

    app_config_lock();
    managed_name_before = app_bt_device_name_is_managed(s_config.bt_device_name);
    if (app_json_find_string(app_http_scratch_buf(), "mode", value, sizeof(value))) {
        s_config.bt_role = app_bt_mode_from_string(value);
        /* 兼容旧字段(0/1):保持同步,便于旧固件回滚与旧接口读值 */
        s_config.bt_mode = (s_config.bt_role == BT_ROLE_OFF) ? BT_MODE_OFF : BT_MODE_BLE;
    }
    if (app_json_find_string(app_http_scratch_buf(), "device_name", value, sizeof(s_config.bt_device_name)) &&
        strlen(value) > 0) {
        has_device_name = true;
        app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), value);
    }
    /* 1.3-59: 只要本次显式给了自定义名字就保留,不再因角色变化回滚成托管名
     * (旧逻辑用 bt_role 与兼容字段 bt_mode 比较,2 != 1 恒真 —— central 下
     * 任何 PUT 都会把自定义扫描前缀重置成本机名,Step 3 双板 E2E 实测踩到)。 */
    if ((has_device_name && app_bt_device_name_is_managed(s_config.bt_device_name)) ||
        (!has_device_name && managed_name_before)) {
        app_set_managed_bt_device_name();
    }
    save_err = app_config_save(&s_config);
    app_config_unlock();

    if (save_err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    /* 蓝牙配置修改统一保存后整机重启:Bluedroid 栈不支持可靠的热重配,
     * 反复 deinit/init 实测会初始化失败(问题与版本优化.md 第 21 项),前端已适配重启提示 */
    app_http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"bluetooth config saved, device will reboot\","
                        "\"restart_required\":true}");
    app_reboot_schedule("bt_config_reboot");
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
        {.uri = "/api/v1/config/network/validate", .method = HTTP_POST,   .handler = network_validate_handler},
        {.uri = "/api/v1/config/network/tryconnect", .method = HTTP_POST, .handler = network_tryconnect_handler},
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
