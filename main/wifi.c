#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"

#include "common.h"
#include "config.h"
#include "identity.h"
#include "mqtt.h"
#include "wifi.h"

static const char *TAG = "wifi";
static const uint8_t APP_WIFI_STA_RETRY_LIMIT = 1;
static const int64_t APP_WIFI_STA_STARTUP_TIMEOUT_MS = 90000;
static const int64_t APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS = 1000;

#define APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS ((unsigned)(sizeof(s_wifi_attempted_mask) * 8U))

extern app_config_t s_config;

esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
wifi_runtime_t s_wifi = {0};
static TaskHandle_t s_wifi_connect_task = NULL;
static TaskHandle_t s_wifi_boot_monitor_task = NULL;
static app_wifi_profile_t s_wifi_profiles[APP_WIFI_PROFILE_MAX];
static size_t s_wifi_profile_count = 0;
static bool s_wifi_profiles_dirty = true;
static int s_wifi_active_profile_index = -1;
static uint8_t s_wifi_attempted_mask = 0;
static int64_t s_wifi_startup_retry_at_ms = 0;
static int64_t s_sta_connected_at_ms = 0;
static bool s_wifi_sta_connecting = false;
static bool s_wifi_fast_path_pending = false;
static SemaphoreHandle_t s_wifi_restart_lock = NULL;

_Static_assert(sizeof(s_wifi.scan_records_storage) >=
                   sizeof(wifi_ap_record_t) * WIFI_SCAN_LIST_SIZE,
               "scan_records_storage must fit WIFI_SCAN_LIST_SIZE records");

_Static_assert(sizeof(TaskHandle_t) == sizeof(((wifi_runtime_t *)0)->restart_task),
               "TaskHandle_t must be same size as restart_task pointer");

static void app_wifi_restart_lock(void)
{
    if (s_wifi_restart_lock == NULL) {
        s_wifi_restart_lock = xSemaphoreCreateRecursiveMutex();
    }
    if (s_wifi_restart_lock != NULL) {
        xSemaphoreTakeRecursive(s_wifi_restart_lock, portMAX_DELAY);
    }
}

static void app_wifi_restart_unlock(void)
{
    if (s_wifi_restart_lock != NULL) {
        xSemaphoreGiveRecursive(s_wifi_restart_lock);
    }
}

#define S_WIFI_SCAN_RECORDS \
    ((wifi_ap_record_t *)s_wifi.scan_records_storage)

void app_wifi_mark_profiles_dirty(void)
{
    app_wifi_profiles_lock();
    s_wifi_profiles_dirty = true;
    app_wifi_profiles_unlock();
}

bool app_wifi_scan_record_at(uint16_t index,
                             const char **ssid_out,
                             int8_t *rssi_out,
                             uint8_t *authmode_out)
{
    wifi_ap_record_t *records = S_WIFI_SCAN_RECORDS;
    bool ok = false;
    app_wifi_runtime_lock();
    if (index < s_wifi.scan_count) {
        if (ssid_out != NULL) {
            *ssid_out = (const char *)records[index].ssid;
        }
        if (rssi_out != NULL) {
            *rssi_out = records[index].rssi;
        }
        if (authmode_out != NULL) {
            *authmode_out = (uint8_t)records[index].authmode;
        }
        ok = true;
    }
    app_wifi_runtime_unlock();
    return ok;
}

void app_wifi_sta_config_updated(void)
{
    app_wifi_mark_profiles_dirty();
    app_wifi_profiles_lock();
    s_wifi_attempted_mask = 0;
    s_wifi_active_profile_index = -1;
    app_wifi_profiles_unlock();
}

const char *app_network_mode_to_string(uint8_t mode)
{
    switch (mode) {
    case NET_MODE_STA:
        return "sta";
    case NET_MODE_APSTA:
        return "apsta";
    case NET_MODE_AP:
    default:
        return "ap";
    }
}

uint8_t app_network_mode_from_string(const char *mode)
{
    if (strcmp(mode, "sta") == 0) {
        return NET_MODE_STA;
    }
    if (strcmp(mode, "apsta") == 0) {
        return NET_MODE_APSTA;
    }
    return NET_MODE_AP;
}

static void app_set_sta_ip_strings(const esp_netif_ip_info_t *ip_info)
{
    app_wifi_runtime_lock();
    if (ip_info == NULL) {
        app_copy_string(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), "0.0.0.0");
        app_copy_string(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), "0.0.0.0");
        app_copy_string(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), "0.0.0.0");
        app_wifi_runtime_unlock();
        return;
    }

    snprintf(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), IPSTR, IP2STR(&ip_info->ip));
    snprintf(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), IPSTR, IP2STR(&ip_info->gw));
    snprintf(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), IPSTR, IP2STR(&ip_info->netmask));
    app_wifi_runtime_unlock();
}

static wifi_mode_t app_network_mode_to_wifi_mode(uint8_t mode)
{
    switch (mode) {
    case NET_MODE_STA:
        return WIFI_MODE_STA;
    case NET_MODE_APSTA:
        return WIFI_MODE_APSTA;
    case NET_MODE_AP:
    default:
        return WIFI_MODE_AP;
    }
}

bool app_network_mode_has_sta(uint8_t mode)
{
    return mode == NET_MODE_STA || mode == NET_MODE_APSTA;
}

bool app_network_mode_has_ap(uint8_t mode)
{
    return mode == NET_MODE_AP || mode == NET_MODE_APSTA;
}

static int64_t app_wifi_now_ms(void)
{
    return esp_timer_get_time() / 1000;
}

static void app_wifi_set_startup_fallback_reason(const char *reason)
{
    app_wifi_runtime_lock();
    app_copy_string(s_wifi.startup_fallback_reason,
                    sizeof(s_wifi.startup_fallback_reason),
                    reason != NULL ? reason : "none");
    app_wifi_runtime_unlock();
}

static void app_wifi_stop_startup_connect_window(void)
{
    app_wifi_runtime_lock();
    s_wifi.startup_connect_active = false;
    s_wifi.startup_connect_deadline_ms = 0;
    s_wifi_fast_path_pending = false;
    app_wifi_runtime_unlock();
    s_wifi_startup_retry_at_ms = 0;
    s_wifi_sta_connecting = false;
}

static void app_wifi_start_startup_connect_window(void)
{
    uint8_t net_mode;
    app_config_lock();
    net_mode = s_config.net_mode;
    app_config_unlock();
    if (net_mode != NET_MODE_STA) {
        app_wifi_stop_startup_connect_window();
        return;
    }

    app_wifi_runtime_lock();
    s_wifi.startup_connect_active = true;
    s_wifi.startup_fallback_to_ap = false;
    s_wifi.startup_connect_deadline_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_TIMEOUT_MS;
    s_wifi_fast_path_pending = true;
    app_wifi_runtime_unlock();
    s_wifi_startup_retry_at_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
    app_wifi_set_startup_fallback_reason("none");
}

static esp_err_t app_configure_ap_netif_ip(void)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err;

    if (s_ap_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK &&
        err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED &&
        err != ESP_ERR_ESP_NETIF_IF_NOT_READY &&
        err != ESP_ERR_INVALID_STATE &&
        err != ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGW(TAG, "dhcps stop unexpected err 0x%x, still proceed to set_ip_info", err);
    }

    IP4_ADDR(&ip_info.ip, 192, 168, 8, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 8, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    err = esp_netif_set_ip_info(s_ap_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "set ap netif ip info failed: %s", esp_err_to_name(err));
        return err;
    }

    err = esp_netif_dhcps_start(s_ap_netif);
    if (err == ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED ||
        err == ESP_ERR_ESP_NETIF_IF_NOT_READY ||
        err == ESP_ERR_INVALID_STATE ||
        err == ESP_ERR_NOT_SUPPORTED) {
        err = ESP_OK;
    }
    return err;
}

static void app_wifi_load_saved_profiles(void)
{
    app_wifi_profile_t stored_profiles[APP_WIFI_PROFILE_MAX] = {0};
    size_t stored_count = 0;
    size_t read_index;
    size_t write_index = 0;
    char cfg_sta_ssid[sizeof(s_config.sta_ssid)];
    char cfg_sta_password[sizeof(s_config.sta_password)];
    bool cfg_sta_has_ssid;

    app_config_lock();
    cfg_sta_has_ssid = (s_config.sta_ssid[0] != '\0');
    if (cfg_sta_has_ssid) {
        app_copy_string(cfg_sta_ssid, sizeof(cfg_sta_ssid), s_config.sta_ssid);
        app_copy_string(cfg_sta_password, sizeof(cfg_sta_password), s_config.sta_password);
    } else {
        cfg_sta_ssid[0] = '\0';
        cfg_sta_password[0] = '\0';
    }
    app_config_unlock();

    app_wifi_profiles_lock();
    if (!s_wifi_profiles_dirty) {
        app_wifi_profiles_unlock();
        return;
    }

    memset(s_wifi_profiles, 0, sizeof(s_wifi_profiles));
    s_wifi_profile_count = 0;
    app_wifi_profiles_unlock();
    if (app_config_load_wifi_profiles(stored_profiles, APP_WIFI_PROFILE_MAX, &stored_count) != ESP_OK) {
        stored_count = 0;
    }
    if (stored_count > APP_WIFI_PROFILE_MAX) {
        stored_count = APP_WIFI_PROFILE_MAX;
    }

    app_wifi_profiles_lock();
    if (cfg_sta_has_ssid) {
        app_copy_string(s_wifi_profiles[write_index].ssid,
                        sizeof(s_wifi_profiles[write_index].ssid),
                        cfg_sta_ssid);
        app_copy_string(s_wifi_profiles[write_index].password,
                        sizeof(s_wifi_profiles[write_index].password),
                        cfg_sta_password);
        write_index++;
    }

    for (read_index = 0;
         read_index < stored_count && write_index < APP_WIFI_PROFILE_MAX;
         read_index++) {
        if (stored_profiles[read_index].ssid[0] == '\0') {
            continue;
        }
        if (cfg_sta_has_ssid &&
            strcmp(stored_profiles[read_index].ssid, cfg_sta_ssid) == 0) {
            continue;
        }
        s_wifi_profiles[write_index] = stored_profiles[read_index];
        write_index++;
    }

    s_wifi_profile_count = write_index;
    s_wifi_profiles_dirty = false;
    app_wifi_profiles_unlock();
}

static int app_wifi_find_best_rssi_for_profile(const char *ssid)
{
    int best_rssi = -128;
    uint16_t index;
    uint16_t scan_count;

    if (ssid == NULL || ssid[0] == '\0') {
        return -128;
    }
    app_wifi_runtime_lock();
    scan_count = s_wifi.scan_count;
    app_wifi_runtime_unlock();
    for (index = 0; index < scan_count; index++) {
        if (strcmp((const char *)S_WIFI_SCAN_RECORDS[index].ssid, ssid) != 0) {
            continue;
        }
        if (S_WIFI_SCAN_RECORDS[index].rssi > best_rssi) {
            best_rssi = S_WIFI_SCAN_RECORDS[index].rssi;
        }
    }
    return best_rssi;
}

static int app_wifi_select_best_profile_index(uint8_t exclude_mask)
{
    int best_index = -1;
    int best_rssi = -128;
    size_t index;
    size_t profile_count;
    size_t mask_bits;

    app_wifi_profiles_lock();
    profile_count = s_wifi_profile_count;
    app_wifi_profiles_unlock();
    mask_bits = APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS;

    for (index = 0;
         index < profile_count && index < mask_bits;
         index++) {
        int rssi;
        const char *ssid;

        if ((exclude_mask & (1U << index)) != 0U) {
            continue;
        }
        app_wifi_profiles_lock();
        ssid = s_wifi_profiles[index].ssid;
        app_wifi_profiles_unlock();
        rssi = app_wifi_find_best_rssi_for_profile(ssid);
        if (rssi <= -128) {
            continue;
        }
        if (best_index < 0 || rssi > best_rssi || (rssi == best_rssi && (int)index < best_index)) {
            best_index = (int)index;
            best_rssi = rssi;
        }
    }
    return best_index;
}

static esp_err_t app_apply_wifi_config(void);
static void app_wifi_handle_startup_sta_failure(const char *reason);
static void app_wifi_schedule_connect_task(TickType_t delay_ticks);

static esp_err_t app_wifi_connect_profile_index(int profile_index)
{
    wifi_config_t sta_cfg = {0};
    esp_err_t err;
    size_t profile_count;
    int active_snapshot;
    bool same_profile;

    app_wifi_profiles_lock();
    profile_count = s_wifi_profile_count;
    active_snapshot = s_wifi_active_profile_index;
    app_wifi_profiles_unlock();

    if (profile_index < 0 || (size_t)profile_index >= profile_count) {
        return ESP_ERR_INVALID_ARG;
    }

    same_profile = (active_snapshot == profile_index);
    if (!same_profile) {
        app_wifi_profiles_lock();
        app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid),
                             s_wifi_profiles[profile_index].ssid);
        app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password),
                             s_wifi_profiles[profile_index].password);
        app_wifi_profiles_unlock();
        sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }

    esp_wifi_disconnect();
    if (!same_profile) {
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            s_wifi_sta_connecting = false;
            ESP_LOGE(TAG, "connect_profile: set sta config failed: %s", esp_err_to_name(err));
            return err;
        }
        app_wifi_profiles_lock();
        s_wifi_active_profile_index = profile_index;
        app_wifi_profiles_unlock();
    }
    app_wifi_runtime_lock();
    s_wifi.sta_retries = 0;
    app_wifi_runtime_unlock();
    s_wifi_sta_connecting = true;
    app_wifi_runtime_lock();
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "switching_profile");
    app_wifi_runtime_unlock();
    return esp_wifi_connect();
}

static esp_err_t app_wifi_connect_best_saved_profile(bool allow_fast_path)
{
    esp_err_t err;
    int profile_index;
    size_t profile_count;
    char sta_ssid_snapshot[33];
    char sta_password_snapshot[65];
    bool sta_has_ssid = false;

    app_wifi_load_saved_profiles();
    app_config_lock();
    if (s_config.sta_ssid[0] != '\0') {
        app_copy_string(sta_ssid_snapshot, sizeof(sta_ssid_snapshot), s_config.sta_ssid);
        app_copy_string(sta_password_snapshot, sizeof(sta_password_snapshot), s_config.sta_password);
        sta_has_ssid = true;
    }
    app_config_unlock();

    if (allow_fast_path && sta_has_ssid) {
        wifi_config_t sta_cfg = {0};
        app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), sta_ssid_snapshot);
        app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), sta_password_snapshot);
        sta_cfg.sta.threshold.authmode =
            (strlen(sta_password_snapshot) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
        sta_cfg.sta.bssid_set = false;
        sta_cfg.sta.channel = 0;
        app_wifi_profiles_lock();
        profile_index = -1;
        if (s_wifi_profile_count > 0) {
            size_t i;
            for (i = 0; i < s_wifi_profile_count; i++) {
                if (strcmp((const char *)s_wifi_profiles[i].ssid, sta_ssid_snapshot) == 0) {
                    profile_index = (int)i;
                    break;
                }
            }
        }
        app_wifi_profiles_unlock();
        esp_wifi_disconnect();
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err == ESP_OK) {
            if (profile_index >= 0) {
                app_wifi_profiles_lock();
                s_wifi_active_profile_index = profile_index;
                app_wifi_profiles_unlock();
            }
            app_wifi_runtime_lock();
            s_wifi.sta_retries = 0;
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = true;
            app_wifi_runtime_lock();
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "fast_path");
            app_wifi_runtime_unlock();
            err = esp_wifi_connect();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "auto_connect: fast_path ssid=%s", sta_ssid_snapshot);
                return ESP_OK;
            }
            ESP_LOGW(TAG, "fast_path connect failed: %s, fall back to scan", esp_err_to_name(err));
        } else {
            ESP_LOGW(TAG, "fast_path set_config failed: %s, fall back to scan", esp_err_to_name(err));
        }
    }

    app_wifi_profiles_lock();
    profile_count = s_wifi_profile_count;
    app_wifi_profiles_unlock();
    if (profile_count == 0) {
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_profile");
        app_wifi_runtime_unlock();
        return ESP_ERR_NOT_FOUND;
    }

    err = app_wifi_perform_scan();
    if (err != ESP_OK) {
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "scan_failed");
        app_wifi_runtime_unlock();
        return err;
    }

    for (;;) {
        uint8_t attempted_mask;
        app_wifi_profiles_lock();
        attempted_mask = s_wifi_attempted_mask;
        app_wifi_profiles_unlock();
        profile_index = app_wifi_select_best_profile_index(attempted_mask);
        if (profile_index < 0) {
            app_wifi_profiles_lock();
            s_wifi_attempted_mask = 0;
            app_wifi_profiles_unlock();
            app_wifi_runtime_lock();
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_ap");
            app_wifi_runtime_unlock();
            return ESP_ERR_NOT_FOUND;
        }
        err = app_wifi_connect_profile_index(profile_index);
        if (err == ESP_OK) {
            app_wifi_profiles_lock();
            ESP_LOGI(TAG, "auto_connect: selected saved profile index=%d ssid=%s",
                     profile_index, s_wifi_profiles[profile_index].ssid);
            app_wifi_profiles_unlock();
            return ESP_OK;
        }
        app_wifi_profiles_lock();
        s_wifi_attempted_mask |= (uint8_t)(1U << profile_index);
        app_wifi_profiles_unlock();
    }
}

static void app_wifi_connect_task_fn(void *arg)
{
    TickType_t delay_ticks = (TickType_t)(uintptr_t)arg;
    bool allow_fast_path = false;

    if (delay_ticks > 0) {
        vTaskDelay(delay_ticks);
    }
    app_wifi_runtime_lock();
    allow_fast_path = s_wifi_fast_path_pending;
    s_wifi_fast_path_pending = false;
    app_wifi_runtime_unlock();
    app_wifi_connect_best_saved_profile(allow_fast_path);
    s_wifi_connect_task = NULL;
    vTaskDelete(NULL);
}

static void app_wifi_handle_startup_sta_failure(const char *reason)
{
    const char *effective_reason = reason;
    char buf_last_disconnect[sizeof(s_wifi.last_disconnect)];
    char buf_fallback_reason[sizeof(s_wifi.startup_fallback_reason)];
    uint8_t buf_net_mode;
    bool buf_fallback_to_ap;

    app_config_lock();
    buf_net_mode = s_config.net_mode;
    app_config_unlock();
    app_wifi_runtime_lock();
    buf_fallback_to_ap = s_wifi.startup_fallback_to_ap;
    app_wifi_runtime_unlock();
    if (buf_net_mode != NET_MODE_STA || buf_fallback_to_ap) {
        return;
    }

    app_wifi_runtime_lock();
    app_copy_string(buf_last_disconnect, sizeof(buf_last_disconnect), s_wifi.last_disconnect);
    app_wifi_runtime_unlock();
    if (buf_last_disconnect[0] != '\0' &&
        strcmp(buf_last_disconnect, "none") != 0 &&
        strcmp(buf_last_disconnect, "switching_profile") != 0) {
        effective_reason = buf_last_disconnect;
    }

    app_wifi_runtime_lock();
    s_wifi.startup_fallback_to_ap = true;
    app_wifi_runtime_unlock();
    app_wifi_set_startup_fallback_reason(effective_reason != NULL ? effective_reason : "connect_timeout");
    app_wifi_stop_startup_connect_window();
    app_wifi_runtime_lock();
    app_copy_string(buf_fallback_reason, sizeof(buf_fallback_reason), s_wifi.startup_fallback_reason);
    app_wifi_runtime_unlock();
    app_wifi_runtime_lock();
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), buf_fallback_reason);
    app_wifi_runtime_unlock();
    ESP_LOGW(TAG, "STA startup failed (%s), switching configured mode to AP",
             buf_fallback_reason);

    app_config_lock();
    s_config.net_mode = NET_MODE_AP;
    if (app_config_save(&s_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist AP fallback mode");
    }
    app_config_unlock();
    if (app_apply_wifi_config() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply AP fallback mode");
    }
}

static void app_wifi_boot_monitor_task_fn(void *arg)
{
    (void)arg;

    while (true) {
        int64_t now_ms = app_wifi_now_ms();
        bool startup_connect_active;
        bool sta_has_ip;
        bool sta_connected;
        bool sta_connecting;
        bool schedule_connect_now = false;
        bool handle_failure = false;
        int64_t deadline_ms;
        uint8_t net_mode;
        TaskHandle_t connect_task_snapshot;

        app_config_lock();
        net_mode = s_config.net_mode;
        app_config_unlock();
        app_wifi_runtime_lock();
        startup_connect_active = s_wifi.startup_connect_active;
        sta_has_ip = s_wifi.sta_has_ip;
        sta_connected = s_wifi.sta_connected;
        deadline_ms = s_wifi.startup_connect_deadline_ms;
        app_wifi_runtime_unlock();
        sta_connecting = s_wifi_sta_connecting;
        connect_task_snapshot = s_wifi_connect_task;

        if (startup_connect_active && net_mode == NET_MODE_STA && !sta_has_ip) {
            if (now_ms >= deadline_ms) {
                handle_failure = true;
            } else if (!sta_connected &&
                       !sta_connecting &&
                       connect_task_snapshot == NULL &&
                       now_ms >= s_wifi_startup_retry_at_ms) {
                schedule_connect_now = true;
            }
        }
        if (handle_failure) {
            app_wifi_handle_startup_sta_failure("connect_timeout");
        } else if (schedule_connect_now) {
            app_wifi_schedule_connect_task(0);
            s_wifi_startup_retry_at_ms = now_ms + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void app_wifi_schedule_connect_task(TickType_t delay_ticks)
{
    uint8_t net_mode;
    app_config_lock();
    net_mode = s_config.net_mode;
    app_config_unlock();
    if (s_wifi_connect_task != NULL || !app_network_mode_has_sta(net_mode)) {
        return;
    }
    if (xTaskCreate(app_wifi_connect_task_fn, "wifi_auto_connect", 4096,
                    (void *)(uintptr_t)delay_ticks, 5, &s_wifi_connect_task) != pdPASS) {
        s_wifi_connect_task = NULL;
        ESP_LOGE(TAG, "Failed to create Wi-Fi auto-connect task");
    }
}

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            app_wifi_profiles_lock();
            s_wifi_attempted_mask = 0;
            app_wifi_profiles_unlock();
            app_wifi_start_startup_connect_window();
            app_wifi_schedule_connect_task(0);
            break;
        case WIFI_EVENT_STA_CONNECTED:
            app_wifi_runtime_lock();
            s_wifi.sta_connected = true;
            s_wifi.sta_retries = 0;
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = false;
            s_wifi_startup_retry_at_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
            s_sta_connected_at_ms = app_wifi_now_ms();
            {
                esp_err_t stop_err = esp_netif_dhcpc_stop(s_sta_netif);
                if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                    ESP_LOGW(TAG, "STA_CONNECTED: dhcpc_stop: %s", esp_err_to_name(stop_err));
                }
                esp_err_t start_err = esp_netif_dhcpc_start(s_sta_netif);
                if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                    ESP_LOGW(TAG, "STA_CONNECTED: dhcpc_start: %s", esp_err_to_name(start_err));
                }
            }
            ESP_LOGI(TAG, "STA_CONNECTED: dhcpc restarted (connected at t=%lldms)",
                     (long long)s_sta_connected_at_ms);
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = event_data;
            uint8_t sta_retries_snapshot;
            int active_profile_snapshot;
            uint8_t net_mode_snapshot;
            bool need_schedule_connect = false;
            bool do_retry_direct = false;

            app_wifi_runtime_lock();
            s_wifi.sta_connected = false;
            s_wifi.sta_has_ip = false;
            sta_retries_snapshot = s_wifi.sta_retries;
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = false;
            app_set_sta_ip_strings(NULL);
            app_wifi_runtime_lock();
            snprintf(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "reason_%d", disconnected->reason);
            app_wifi_runtime_unlock();
            app_mqtt_stop();

            app_config_lock();
            net_mode_snapshot = s_config.net_mode;
            app_config_unlock();
            app_wifi_profiles_lock();
            active_profile_snapshot = s_wifi_active_profile_index;
            app_wifi_profiles_unlock();

            if (app_network_mode_has_sta(net_mode_snapshot) &&
                active_profile_snapshot >= 0 &&
                sta_retries_snapshot < APP_WIFI_STA_RETRY_LIMIT) {
                app_wifi_runtime_lock();
                s_wifi.sta_retries = sta_retries_snapshot + 1U;
                app_wifi_runtime_unlock();
                s_wifi_sta_connecting = true;
                do_retry_direct = true;
            } else if (app_network_mode_has_sta(net_mode_snapshot)) {
                if (active_profile_snapshot >= 0 &&
                    (unsigned)active_profile_snapshot < APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS) {
                    app_wifi_profiles_lock();
                    s_wifi_attempted_mask |= (uint8_t)(1U << active_profile_snapshot);
                    app_wifi_profiles_unlock();
                }
                need_schedule_connect = true;
            }
            if (do_retry_direct) {
                esp_wifi_connect();
            } else if (need_schedule_connect) {
                app_wifi_schedule_connect_task(pdMS_TO_TICKS(250));
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        int active_profile_snapshot;
        size_t profile_count_snapshot;

        app_wifi_runtime_lock();
        s_wifi.sta_connected = true;
        s_wifi.sta_has_ip = true;
        s_wifi.sta_retries = 0;
        app_wifi_runtime_unlock();
        s_wifi_sta_connecting = false;
        app_wifi_profiles_lock();
        s_wifi_attempted_mask = 0;
        app_wifi_profiles_unlock();
        app_set_sta_ip_strings(&event->ip_info);
        app_wifi_stop_startup_connect_window();
        app_wifi_set_startup_fallback_reason("none");
        ESP_LOGI(TAG,
                 "IP_EVENT_STA_GOT_IP: %s (t=%lldms from boot, %lldms from connected)",
                 s_wifi.sta_ip, (long long)app_wifi_now_ms(),
                 s_sta_connected_at_ms > 0 ? (long long)(app_wifi_now_ms() - s_sta_connected_at_ms) : 0LL);

        app_wifi_profiles_lock();
        active_profile_snapshot = s_wifi_active_profile_index;
        profile_count_snapshot = s_wifi_profile_count;
        app_wifi_profiles_unlock();
        if (active_profile_snapshot >= 0 &&
            (size_t)active_profile_snapshot < profile_count_snapshot) {
            char profile_ssid[33];
            char profile_password[65];
            app_wifi_profiles_lock();
            app_copy_string(profile_ssid, sizeof(profile_ssid),
                            s_wifi_profiles[active_profile_snapshot].ssid);
            app_copy_string(profile_password, sizeof(profile_password),
                            s_wifi_profiles[active_profile_snapshot].password);
            app_wifi_profiles_unlock();
            app_config_lock();
            app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), profile_ssid);
            app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), profile_password);
            app_config_save(&s_config);
            app_config_unlock();
            app_config_save_wifi_profile(profile_ssid, profile_password);
            app_wifi_load_saved_profiles();
        }
        if (!app_mqtt_schedule_restart()) {
            ESP_LOGE(TAG, "IP_EVENT_STA_GOT_IP: failed to schedule MQTT restart");
        }
    }
}

static esp_err_t app_apply_wifi_config(void)
{
    wifi_config_t ap_cfg = {0};
    wifi_config_t sta_cfg = {0};
    esp_err_t err;
    uint8_t net_mode_snapshot;

    app_config_lock();
    if (app_network_mode_has_ap(s_config.net_mode)) {
        app_copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), s_config.ap_ssid);
        ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
        app_copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), s_config.ap_password);
    }
    if (app_network_mode_has_sta(s_config.net_mode)) {
        app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), s_config.sta_ssid);
        app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), s_config.sta_password);
    }
    net_mode_snapshot = s_config.net_mode;
    app_config_unlock();

    if (app_network_mode_has_ap(net_mode_snapshot)) {
        ap_cfg.ap.channel = 1;
        ap_cfg.ap.max_connection = 4;
        ap_cfg.ap.authmode = strlen((char *)ap_cfg.ap.password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
        ap_cfg.ap.pmf_cfg.required = false;
    }
    if (app_network_mode_has_sta(net_mode_snapshot)) {
        sta_cfg.sta.threshold.authmode =
            (strlen((const char *)sta_cfg.sta.password) >= 8) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    }

    ESP_LOGI(TAG, "apply_wifi_config: net_mode=%d ap_ssid=%s sta_ssid=%s",
             net_mode_snapshot,
             app_network_mode_has_ap(net_mode_snapshot) ? (const char *)ap_cfg.ap.ssid : "<disabled>",
             app_network_mode_has_sta(net_mode_snapshot) ? (const char *)sta_cfg.sta.ssid : "<disabled>");
    app_mqtt_stop();
    app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "waiting_sta_ip");

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    app_wifi_runtime_lock();
    s_wifi.sta_connected = false;
    s_wifi.sta_has_ip = false;
    s_wifi.sta_retries = 0;
    s_wifi.startup_fallback_to_ap = false;
    app_wifi_runtime_unlock();
    s_wifi_sta_connecting = false;
    app_wifi_profiles_lock();
    s_wifi_active_profile_index = -1;
    s_wifi_attempted_mask = 0;
    app_wifi_profiles_unlock();
    app_wifi_stop_startup_connect_window();
    app_wifi_set_startup_fallback_reason("none");
    app_set_sta_ip_strings(NULL);

    err = esp_wifi_set_mode(app_network_mode_to_wifi_mode(net_mode_snapshot));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_wifi_config: set_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    if (!app_network_mode_has_ap(net_mode_snapshot)) {
        esp_netif_ip_info_t zero_ip;
        memset(&zero_ip, 0, sizeof(zero_ip));
        esp_netif_set_ip_info(s_ap_netif, &zero_ip);
        wifi_config_t zero_ap_cfg = {0};
        zero_ap_cfg.ap.authmode = WIFI_AUTH_OPEN;
        zero_ap_cfg.ap.ssid_len = 0;
        esp_wifi_set_config(WIFI_IF_AP, &zero_ap_cfg);
    }
    if (app_network_mode_has_ap(net_mode_snapshot)) {
        err = esp_wifi_set_config(WIFI_IF_AP, &ap_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply_wifi_config: set ap config failed: %s", esp_err_to_name(err));
            return err;
        }
        err = app_configure_ap_netif_ip();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply_wifi_config: configure ap netif ip failed: %s", esp_err_to_name(err));
            return err;
        }
    }
    if (app_network_mode_has_sta(net_mode_snapshot)) {
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply_wifi_config: set sta config failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    ESP_LOGI(TAG, "apply_wifi_config: calling esp_wifi_start...");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_wifi_config: start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "apply_wifi_config: esp_wifi_start returned OK");
    return ESP_OK;
}

esp_err_t app_wifi_perform_scan(void)
{
    wifi_mode_t old_mode = WIFI_MODE_NULL;
    bool temporary_apsta = false;
    uint16_t number = WIFI_SCAN_LIST_SIZE;
    wifi_scan_config_t scan_cfg = {0};
    esp_err_t err;

    err = esp_wifi_get_mode(&old_mode);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: get_mode failed: %s", esp_err_to_name(err));
        return err;
    }
    if (old_mode == WIFI_MODE_AP) {
        err = esp_wifi_set_mode(WIFI_MODE_APSTA);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "scan: set APSTA failed: %s", esp_err_to_name(err));
            return err;
        }
        temporary_apsta = true;
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    scan_cfg.ssid = NULL;
    scan_cfg.bssid = NULL;
    scan_cfg.channel = 0;
    scan_cfg.show_hidden = false;
    scan_cfg.scan_type = WIFI_SCAN_TYPE_ACTIVE;
    scan_cfg.scan_time.active.min = 50;
    scan_cfg.scan_time.active.max = 100;

    err = esp_wifi_scan_start(&scan_cfg, true);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: start failed: %s", esp_err_to_name(err));
        if (temporary_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return err;
    }
    err = esp_wifi_scan_get_ap_records(&number, S_WIFI_SCAN_RECORDS);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "scan: get_ap_records failed: %s", esp_err_to_name(err));
        if (temporary_apsta) {
            esp_wifi_set_mode(WIFI_MODE_AP);
        }
        return err;
    }
    app_wifi_runtime_lock();
    s_wifi.scan_count = number;
    app_wifi_runtime_unlock();

    if (temporary_apsta) {
        err = esp_wifi_set_mode(WIFI_MODE_AP);
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "scan: restore AP mode failed: %s", esp_err_to_name(err));
        }
    }
    return ESP_OK;
}

static void app_wifi_restart_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        BaseType_t notified;
        notified = xTaskNotifyWaitIndexed(0, 0, ULONG_MAX, NULL, portMAX_DELAY);
        if (notified != pdPASS) {
            continue;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
        if (app_apply_wifi_config() != ESP_OK) {
            ESP_LOGE(TAG, "Wi-Fi reconfigure failed");
        }
    }
}

bool app_wifi_schedule_restart(void)
{
    TaskHandle_t h;
    BaseType_t rt;
    app_wifi_restart_lock();
    h = (TaskHandle_t)s_wifi.restart_task;
    if (h != NULL) {
        app_wifi_restart_unlock();
        xTaskNotifyGive(h);
        return true;
    }
    app_wifi_restart_unlock();
    rt = xTaskCreate(app_wifi_restart_task_fn, "wifi_restart", 4096, NULL, 5, &h);
    if (rt != pdPASS) {
        ESP_LOGE(TAG, "Failed to create wifi_restart task");
        return false;
    }
    app_wifi_restart_lock();
    if (s_wifi.restart_task == NULL) {
        s_wifi.restart_task = h;
    }
    app_wifi_restart_unlock();
    xTaskNotifyGive(h);
    return true;
}

void app_wifi_restart_task(void *arg)
{
    (void)arg;
    app_wifi_schedule_restart();
}

void app_start_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    const char *hostname = app_get_hostname();
    uint8_t net_mode_snapshot;

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    ESP_ERROR_CHECK(esp_netif_set_hostname(s_ap_netif, hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, hostname));

    app_config_lock();
    net_mode_snapshot = s_config.net_mode;
    app_config_unlock();
    if (app_network_mode_has_ap(net_mode_snapshot)) {
        esp_err_t err = app_configure_ap_netif_ip();
        if (err != ESP_OK) {
            ESP_LOGW(TAG, "initial ap netif ip configure skipped (err=0x%x)", err);
        }
    }
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL));
    app_wifi_mark_profiles_dirty();
    if (s_wifi_boot_monitor_task == NULL) {
        ESP_ERROR_CHECK(xTaskCreate(app_wifi_boot_monitor_task_fn, "wifi_boot_monitor", 4096,
                                    NULL, 5, &s_wifi_boot_monitor_task) == pdPASS ? ESP_OK : ESP_FAIL);
    }
    ESP_ERROR_CHECK(app_apply_wifi_config());
}
