#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
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
static const int64_t APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS = 5000;

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
static bool s_wifi_sta_connecting = false;

_Static_assert(sizeof(s_wifi.scan_records_storage) >=
                   sizeof(wifi_ap_record_t) * WIFI_SCAN_LIST_SIZE,
               "scan_records_storage must fit WIFI_SCAN_LIST_SIZE records");

#define S_WIFI_SCAN_RECORDS \
    ((wifi_ap_record_t *)s_wifi.scan_records_storage)

void app_wifi_mark_profiles_dirty(void)
{
    s_wifi_profiles_dirty = true;
}

bool app_wifi_scan_record_at(uint16_t index,
                             const char **ssid_out,
                             int8_t *rssi_out,
                             uint8_t *authmode_out)
{
    wifi_ap_record_t *records = S_WIFI_SCAN_RECORDS;
    if (index >= s_wifi.scan_count) {
        return false;
    }
    if (ssid_out != NULL) {
        *ssid_out = (const char *)records[index].ssid;
    }
    if (rssi_out != NULL) {
        *rssi_out = records[index].rssi;
    }
    if (authmode_out != NULL) {
        *authmode_out = (uint8_t)records[index].authmode;
    }
    return true;
}

void app_wifi_sta_config_updated(void)
{
    app_wifi_mark_profiles_dirty();
    s_wifi_attempted_mask = 0;
    s_wifi_active_profile_index = -1;
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
    if (ip_info == NULL) {
        app_copy_string(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), "0.0.0.0");
        app_copy_string(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), "0.0.0.0");
        app_copy_string(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), "0.0.0.0");
        return;
    }

    snprintf(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), IPSTR, IP2STR(&ip_info->ip));
    snprintf(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), IPSTR, IP2STR(&ip_info->gw));
    snprintf(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), IPSTR, IP2STR(&ip_info->netmask));
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
    app_copy_string(s_wifi.startup_fallback_reason,
                    sizeof(s_wifi.startup_fallback_reason),
                    reason != NULL ? reason : "none");
}

static void app_wifi_stop_startup_connect_window(void)
{
    s_wifi.startup_connect_active = false;
    s_wifi.startup_connect_deadline_ms = 0;
    s_wifi_startup_retry_at_ms = 0;
    s_wifi_sta_connecting = false;
}

static void app_wifi_start_startup_connect_window(void)
{
    if (s_config.net_mode != NET_MODE_STA) {
        app_wifi_stop_startup_connect_window();
        return;
    }

    s_wifi.startup_connect_active = true;
    s_wifi.startup_fallback_to_ap = false;
    s_wifi.startup_connect_deadline_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_TIMEOUT_MS;
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
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));

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

    if (!s_wifi_profiles_dirty) {
        return;
    }

    memset(s_wifi_profiles, 0, sizeof(s_wifi_profiles));
    s_wifi_profile_count = 0;
    if (app_config_load_wifi_profiles(stored_profiles, APP_WIFI_PROFILE_MAX, &stored_count) != ESP_OK) {
        stored_count = 0;
    }
    if (stored_count > APP_WIFI_PROFILE_MAX) {
        stored_count = APP_WIFI_PROFILE_MAX;
    }

    if (s_config.sta_ssid[0] != '\0') {
        app_copy_string(s_wifi_profiles[write_index].ssid,
                        sizeof(s_wifi_profiles[write_index].ssid),
                        s_config.sta_ssid);
        app_copy_string(s_wifi_profiles[write_index].password,
                        sizeof(s_wifi_profiles[write_index].password),
                        s_config.sta_password);
        write_index++;
    }

    for (read_index = 0;
         read_index < stored_count && write_index < APP_WIFI_PROFILE_MAX;
         read_index++) {
        if (stored_profiles[read_index].ssid[0] == '\0') {
            continue;
        }
        if (s_config.sta_ssid[0] != '\0' &&
            strcmp(stored_profiles[read_index].ssid, s_config.sta_ssid) == 0) {
            continue;
        }
        s_wifi_profiles[write_index] = stored_profiles[read_index];
        write_index++;
    }

    s_wifi_profile_count = write_index;
    s_wifi_profiles_dirty = false;
}

static int app_wifi_find_best_rssi_for_profile(const char *ssid)
{
    int best_rssi = -128;
    uint16_t index;

    if (ssid == NULL || ssid[0] == '\0') {
        return -128;
    }
    for (index = 0; index < s_wifi.scan_count; index++) {
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

    for (index = 0;
         index < s_wifi_profile_count && index < APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS;
         index++) {
        int rssi;

        if ((exclude_mask & (1U << index)) != 0U) {
            continue;
        }
        rssi = app_wifi_find_best_rssi_for_profile(s_wifi_profiles[index].ssid);
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

    if (profile_index < 0 || (size_t)profile_index >= s_wifi_profile_count) {
        return ESP_ERR_INVALID_ARG;
    }

    app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid),
                         s_wifi_profiles[profile_index].ssid);
    app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password),
                         s_wifi_profiles[profile_index].password);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    esp_wifi_disconnect();
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    s_wifi_active_profile_index = profile_index;
    s_wifi.sta_retries = 0;
    s_wifi_sta_connecting = true;
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "switching_profile");
    return esp_wifi_connect();
}

static esp_err_t app_wifi_connect_best_saved_profile(void)
{
    esp_err_t err;
    int profile_index;

    app_wifi_load_saved_profiles();
    if (s_wifi_profile_count == 0) {
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_profile");
        return ESP_ERR_NOT_FOUND;
    }

    err = app_wifi_perform_scan();
    if (err != ESP_OK) {
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "scan_failed");
        return err;
    }

    for (;;) {
        profile_index = app_wifi_select_best_profile_index(s_wifi_attempted_mask);
        if (profile_index < 0) {
            // All saved profiles have been tried in this round. Clear the mask so the
            // startup window can begin a new retry round instead of giving up forever.
            s_wifi_attempted_mask = 0;
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_ap");
            return ESP_ERR_NOT_FOUND;
        }
        err = app_wifi_connect_profile_index(profile_index);
        if (err == ESP_OK) {
            return ESP_OK;
        }
        s_wifi_attempted_mask |= (uint8_t)(1U << profile_index);
    }
}

static void app_wifi_connect_task_fn(void *arg)
{
    TickType_t delay_ticks = (TickType_t)(uintptr_t)arg;

    if (delay_ticks > 0) {
        vTaskDelay(delay_ticks);
    }
    app_wifi_connect_best_saved_profile();
    s_wifi_connect_task = NULL;
    vTaskDelete(NULL);
}

static void app_wifi_handle_startup_sta_failure(const char *reason)
{
    const char *effective_reason = reason;

    if (s_config.net_mode != NET_MODE_STA || s_wifi.startup_fallback_to_ap) {
        return;
    }

    if (s_wifi.last_disconnect[0] != '\0' &&
        strcmp(s_wifi.last_disconnect, "none") != 0 &&
        strcmp(s_wifi.last_disconnect, "switching_profile") != 0) {
        effective_reason = s_wifi.last_disconnect;
    }

    s_wifi.startup_fallback_to_ap = true;
    app_wifi_set_startup_fallback_reason(effective_reason != NULL ? effective_reason : "connect_timeout");
    app_wifi_stop_startup_connect_window();
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), s_wifi.startup_fallback_reason);
    ESP_LOGW(TAG, "STA startup failed (%s), switching configured mode to AP",
             s_wifi.startup_fallback_reason);

    s_config.net_mode = NET_MODE_AP;
    if (app_config_save(&s_config) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to persist AP fallback mode");
    }
    if (app_apply_wifi_config() != ESP_OK) {
        ESP_LOGE(TAG, "Failed to apply AP fallback mode");
    }
}

static void app_wifi_boot_monitor_task_fn(void *arg)
{
    (void)arg;

    while (true) {
        int64_t now_ms = app_wifi_now_ms();

        if (s_wifi.startup_connect_active && s_config.net_mode == NET_MODE_STA && !s_wifi.sta_has_ip) {
            if (now_ms >= s_wifi.startup_connect_deadline_ms) {
                app_wifi_handle_startup_sta_failure("connect_timeout");
            } else if (!s_wifi.sta_connected &&
                       !s_wifi_sta_connecting &&
                       s_wifi_connect_task == NULL &&
                       now_ms >= s_wifi_startup_retry_at_ms) {
                app_wifi_schedule_connect_task(0);
                s_wifi_startup_retry_at_ms = now_ms + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
            }
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

static void app_wifi_schedule_connect_task(TickType_t delay_ticks)
{
    if (s_wifi_connect_task != NULL || !app_network_mode_has_sta(s_config.net_mode)) {
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
            s_wifi_attempted_mask = 0;
            app_wifi_start_startup_connect_window();
            app_wifi_schedule_connect_task(pdMS_TO_TICKS(100));
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_wifi.sta_connected = true;
            s_wifi.sta_retries = 0;
            s_wifi_sta_connecting = false;
            s_wifi_startup_retry_at_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = event_data;
            s_wifi.sta_connected = false;
            s_wifi.sta_has_ip = false;
            s_wifi_sta_connecting = false;
            app_set_sta_ip_strings(NULL);
            snprintf(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "reason_%d", disconnected->reason);
            app_mqtt_stop();
            if (app_network_mode_has_sta(s_config.net_mode) &&
                s_wifi_active_profile_index >= 0 &&
                s_wifi.sta_retries < APP_WIFI_STA_RETRY_LIMIT) {
                s_wifi.sta_retries++;
                s_wifi_sta_connecting = true;
                esp_wifi_connect();
            } else if (app_network_mode_has_sta(s_config.net_mode)) {
                if (s_wifi_active_profile_index >= 0 &&
                    (unsigned)s_wifi_active_profile_index < APP_WIFI_PROFILE_ATTEMPTED_MASK_BITS) {
                    s_wifi_attempted_mask |= (uint8_t)(1U << s_wifi_active_profile_index);
                }
                app_wifi_schedule_connect_task(pdMS_TO_TICKS(250));
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        s_wifi.sta_connected = true;
        s_wifi.sta_has_ip = true;
        s_wifi.sta_retries = 0;
        s_wifi_sta_connecting = false;
        s_wifi_attempted_mask = 0;
        app_set_sta_ip_strings(&event->ip_info);
        app_wifi_stop_startup_connect_window();
        app_wifi_set_startup_fallback_reason("none");
        if (s_wifi_active_profile_index >= 0 &&
            (size_t)s_wifi_active_profile_index < s_wifi_profile_count) {
            app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid),
                            s_wifi_profiles[s_wifi_active_profile_index].ssid);
            app_copy_string(s_config.sta_password, sizeof(s_config.sta_password),
                            s_wifi_profiles[s_wifi_active_profile_index].password);
            app_config_save(&s_config);
            app_config_save_wifi_profile(s_config.sta_ssid, s_config.sta_password);
            app_wifi_load_saved_profiles();
        }
        xTaskCreate(app_mqtt_restart_task, "mqtt_restart_ip", 4096, NULL, 5, NULL);
    }
}

static esp_err_t app_apply_wifi_config(void)
{
    wifi_config_t ap_cfg = {0};
    wifi_config_t sta_cfg = {0};
    esp_err_t err;

    app_copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), s_config.ap_ssid);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    app_copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), s_config.ap_password);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(s_config.ap_password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), s_config.sta_ssid);
    app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), s_config.sta_password);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    app_mqtt_stop();
    app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "waiting_sta_ip");

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    s_wifi.sta_connected = false;
    s_wifi.sta_has_ip = false;
    s_wifi.sta_retries = 0;
    s_wifi_sta_connecting = false;
    s_wifi_active_profile_index = -1;
    s_wifi_attempted_mask = 0;
    s_wifi.startup_fallback_to_ap = false;
    app_wifi_stop_startup_connect_window();
    app_wifi_set_startup_fallback_reason("none");
    app_set_sta_ip_strings(NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(app_network_mode_to_wifi_mode(s_config.net_mode)));
    if (app_network_mode_has_ap(s_config.net_mode)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(app_configure_ap_netif_ip());
    }
    if (app_network_mode_has_sta(s_config.net_mode)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    return ESP_OK;
}

esp_err_t app_wifi_perform_scan(void)
{
    wifi_mode_t old_mode = WIFI_MODE_NULL;
    bool temporary_apsta = false;
    uint16_t number = WIFI_SCAN_LIST_SIZE;

    ESP_ERROR_CHECK(esp_wifi_get_mode(&old_mode));
    if (old_mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        temporary_apsta = true;
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, S_WIFI_SCAN_RECORDS));
    s_wifi.scan_count = number;

    if (temporary_apsta) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }
    return ESP_OK;
}

void app_wifi_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(250));
    if (app_apply_wifi_config() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi reconfigure failed");
    }
    vTaskDelete(NULL);
}

void app_start_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    const char *hostname = app_get_hostname();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_ap_netif, hostname));
    ESP_ERROR_CHECK(esp_netif_set_hostname(s_sta_netif, hostname));
    {
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
