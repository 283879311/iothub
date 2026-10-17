#include <stdio.h>
#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_netif_defaults.h"
#include "esp_netif_net_stack.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "esp_wifi_default.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/etharp.h"
#include "lwip/ip4_addr.h"

#include "dhcpserver/dhcpserver.h"

#include "common.h"
#include "config.h"
#include "identity.h"
#include "wifi.h"

static const char *TAG = "wifi";
/* 同一 profile 断线后先直连重试的次数,超过才拉黑换下一个 profile。
 * 取 3 避免边缘 AP 一次抖动就永久跳走;总时长仍受 90s 启动窗口约束。 */
static const uint8_t APP_WIFI_STA_RETRY_LIMIT = 3;
static const int64_t APP_WIFI_STA_STARTUP_TIMEOUT_MS = 90000;
static const int64_t APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS = 1000;
/* STA 关联的最低可接受安全模式(1.3-04 实测):原实现按"口令 >= 8 位"设 WIFI_AUTH_WPA2_PSK,
 * 会拒连安全模式判定低于 WPA2 的上游(WPA-only / 部分混模路由器),报 reason=210
 * (NO_AP_FOUND_W_COMPATIBLE_SECURITY:找到了 AP 但安全模式与阈值不兼容)。
 * 阈值只作"最低可接受"用,WPA_PSK 已足以挡住开放/弱加密 AP;量产若需收紧改此一处即可。 */
#define APP_WIFI_STA_AUTHMODE_MIN WIFI_AUTH_WPA_PSK

/* 前向声明:该函数定义在断连原因码表附近(约 800 行),而连接路径(约 600 行)先用到它 */
static const char *app_wifi_auth_name(uint8_t auth);
static void app_wifi_log_sta_cfg(const char *where, const wifi_config_t *cfg);
static esp_err_t app_wifi_retry_with_pmf_required(void);

/* 210(NO_AP_FOUND_W_COMPAT_SECURITY)兜底:首轮 required=false,失败后带 required=true 只重试一次 */
static bool s_wifi_pmf_required_tried = false;

/* 试连验证状态(1.3-37):试连期间事件处理器只记结果、不跑正常自动重连逻辑 */
static bool s_tryconnect_active = false;
static bool s_tryconnect_connecting = false;
static app_wifi_try_result_t s_tryconnect_result = APP_WIFI_TRY_TIMEOUT;
static EventGroupHandle_t s_tryconnect_events = NULL;
static StaticEventGroup_t s_tryconnect_events_buf;
static portMUX_TYPE s_tryconnect_mux = portMUX_INITIALIZER_UNLOCKED;
#define APP_WIFI_TRY_DONE_BIT BIT0

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
static StaticSemaphore_t s_wifi_restart_lock_buf;
static portMUX_TYPE s_wifi_restart_lock_mux = portMUX_INITIALIZER_UNLOCKED;

_Static_assert(sizeof(s_wifi.scan_records_storage) >=
                   sizeof(wifi_ap_record_t) * WIFI_SCAN_LIST_SIZE,
               "scan_records_storage must fit WIFI_SCAN_LIST_SIZE records");

_Static_assert(sizeof(TaskHandle_t) == sizeof(((wifi_runtime_t *)0)->restart_task),
               "TaskHandle_t must be same size as restart_task pointer");

static void app_wifi_restart_lock(void)
{
    SemaphoreHandle_t lock = app_mutex_ensure(&s_wifi_restart_lock, &s_wifi_restart_lock_buf,
                                              &s_wifi_restart_lock_mux, true);
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
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

bool app_wifi_scan_record_bssid(uint16_t index, char *out, size_t out_size)
{
    wifi_ap_record_t *records = S_WIFI_SCAN_RECORDS;
    bool ok = false;

    if (out == NULL || out_size < 18) {
        return false;
    }
    out[0] = '\0';
    app_wifi_runtime_lock();
    if (index < s_wifi.scan_count) {
        const uint8_t *b = records[index].bssid;
        snprintf(out, out_size, "%02x:%02x:%02x:%02x:%02x:%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5]);
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

/* ---------------------------------------------------------------------------
 * 静态 IP 冲突检测(RFC 5227 简化版):拿到静态 IP 且 STA 连上后,对自身 IP
 * 发 ARP 请求;若局域网里有其它设备应答(ARP 表里出现非自身 MAC 的条目),
 * 即判定冲突,置 s_wifi.sta_ip_conflict —— 状态接口透出,前端弹窗提示改址。
 * ------------------------------------------------------------------------- */
static TaskHandle_t s_ip_conflict_task = NULL;

static void app_sta_ip_conflict_check_task(void *arg)
{
    (void)arg;
    esp_netif_ip_info_t ip_info;
    ip4_addr_t probe_ip;
    struct eth_addr *entry_mac = NULL;
    const ip4_addr_t *entry_ip = NULL;
    uint8_t own_mac[6] = {0};
    char ip_str[16];
    bool conflict = false;
    int i;

    vTaskDelay(pdMS_TO_TICKS(400));
    if (s_sta_netif == NULL ||
        esp_netif_get_ip_info(s_sta_netif, &ip_info) != ESP_OK) {
        goto done;
    }
    {
        struct netif *netif_lwip = (struct netif *)esp_netif_get_netif_impl(s_sta_netif);

        if (netif_lwip == NULL) {
            goto done;
        }
        probe_ip.addr = ip_info.ip.addr;
        for (i = 0; i < 3; i++) {
            etharp_request(netif_lwip, &probe_ip);
            vTaskDelay(pdMS_TO_TICKS(400));
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
        snprintf(ip_str, sizeof(ip_str), IPSTR, IP2STR(&ip_info.ip));
        if (etharp_find_addr(netif_lwip, &probe_ip, &entry_mac, &entry_ip) >= 0 &&
            entry_mac != NULL) {
            if (esp_wifi_get_mac(WIFI_IF_STA, own_mac) == ESP_OK &&
                memcmp(entry_mac->addr, own_mac, sizeof(own_mac)) == 0) {
                ESP_LOGI(TAG, "static ip %s: ARP entry is own MAC, no conflict", ip_str);
            } else {
                conflict = true;
                ESP_LOGW(TAG, "static ip CONFLICT: %s answered by %02x:%02x:%02x:%02x:%02x:%02x",
                         ip_str, entry_mac->addr[0], entry_mac->addr[1], entry_mac->addr[2],
                         entry_mac->addr[3], entry_mac->addr[4], entry_mac->addr[5]);
            }
        } else {
            ESP_LOGI(TAG, "static ip %s: no ARP answer, no conflict", ip_str);
        }
    }

done:
    app_wifi_runtime_lock();
    s_wifi.sta_ip_conflict = conflict;
    app_wifi_runtime_unlock();
    s_ip_conflict_task = NULL;
    vTaskDelete(NULL);
}

static void app_sta_ip_conflict_check_start(void)
{
    if (s_ip_conflict_task != NULL) {
        return;
    }
    if (xTaskCreate(app_sta_ip_conflict_check_task, "ip_conflict", 2560, NULL, 3,
                    &s_ip_conflict_task) != pdPASS) {
        s_ip_conflict_task = NULL;
        ESP_LOGW(TAG, "Failed to create static ip conflict check task");
    }
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

static bool app_parse_ipv4_str(const char *s, esp_ip4_addr_t *out)
{
    ip4_addr_t parsed;

    if (s == NULL || out == NULL) {
        return false;
    }
    if (!ip4addr_aton(s, &parsed) || ip4_addr_isany_val(parsed)) {
        return false;
    }
    out->addr = parsed.addr;
    return true;
}

static esp_err_t app_configure_sta_static_ip(void)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_dns_info_t dns_info = {0};
    char ip_str[sizeof(s_config.sta_static_ip)];
    char gw_str[sizeof(s_config.sta_static_gateway)];
    char mask_str[sizeof(s_config.sta_static_netmask)];
    char dns1_str[sizeof(s_config.sta_static_dns1)];
    char dns2_str[sizeof(s_config.sta_static_dns2)];
    esp_err_t err;

    if (s_sta_netif == NULL) {
        return ESP_ERR_INVALID_STATE;
    }

    app_config_lock();
    app_copy_string(ip_str, sizeof(ip_str), s_config.sta_static_ip);
    app_copy_string(gw_str, sizeof(gw_str), s_config.sta_static_gateway);
    app_copy_string(mask_str, sizeof(mask_str), s_config.sta_static_netmask);
    app_copy_string(dns1_str, sizeof(dns1_str), s_config.sta_static_dns1);
    app_copy_string(dns2_str, sizeof(dns2_str), s_config.sta_static_dns2);
    app_config_unlock();

    if (!app_parse_ipv4_str(ip_str, &ip_info.ip) ||
        !app_parse_ipv4_str(gw_str, &ip_info.gw) ||
        !app_parse_ipv4_str(mask_str, &ip_info.netmask)) {
        ESP_LOGE(TAG, "static ip parse failed");
        return ESP_ERR_INVALID_ARG;
    }

    err = esp_netif_dhcpc_stop(s_sta_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        ESP_LOGW(TAG, "static ip: dhcpc_stop: %s", esp_err_to_name(err));
    }

    err = esp_netif_set_ip_info(s_sta_netif, &ip_info);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "static ip: set_ip_info failed: %s", esp_err_to_name(err));
        return err;
    }

    if (dns1_str[0] != '\0' && app_parse_ipv4_str(dns1_str, &dns_info.ip.u_addr.ip4)) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_MAIN, &dns_info);
    }
    if (dns2_str[0] != '\0' && app_parse_ipv4_str(dns2_str, &dns_info.ip.u_addr.ip4)) {
        dns_info.ip.type = ESP_IPADDR_TYPE_V4;
        esp_netif_set_dns_info(s_sta_netif, ESP_NETIF_DNS_BACKUP, &dns_info);
    }

    ESP_LOGI(TAG, "static ip configured: ip=%s gw=%s mask=%s dns1=%s dns2=%s",
             ip_str, gw_str, mask_str,
             dns1_str[0] != '\0' ? dns1_str : "(none)",
             dns2_str[0] != '\0' ? dns2_str : "(none)");
    return ESP_OK;
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

    /* DHCP 下发 DNS=AP IP:客户端的域名查询才会进 captive dns 劫持
     * (IDF dhcps 默认不添加 DNS 选项,须同时置位 OFFER_DNS 与地址,问题与版本优化.md 第 24 项) */
    dhcps_offer_t dns_offer = OFFER_DNS;
    esp_netif_dns_info_t dns_info = {0};
    dns_info.ip.type = ESP_IPADDR_TYPE_V4;
    IP4_ADDR(&dns_info.ip.u_addr.ip4, 192, 168, 8, 1);
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_DOMAIN_NAME_SERVER,
                           &dns_offer, sizeof(dns_offer));
    esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns_info);

    /* DHCP 选项 114(强制门户地址,RFC 7710):较新系统跳过探测直接打开此 URL,
     * 必须免认证 200——指向落地页而非会 401 的根路径;URI 按 RFC 不带 NUL 尾 */
    static const char captive_portal_uri[] = "http://192.168.8.1" APP_WEB_PORTAL_PATH;
    esp_netif_dhcps_option(s_ap_netif, ESP_NETIF_OP_SET, ESP_NETIF_CAPTIVEPORTAL_URI,
                           (void *)captive_portal_uri, sizeof(captive_portal_uri) - 1);

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
        sta_cfg.sta.threshold.authmode =
            (strlen((const char *)sta_cfg.sta.password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;
    }

    esp_wifi_disconnect();
    if (!same_profile) {
        app_wifi_log_sta_cfg("connect_profile", &sta_cfg);
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
            (strlen(sta_password_snapshot) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
        sta_cfg.sta.bssid_set = false;
        sta_cfg.sta.channel = 0;
        app_wifi_log_sta_cfg("fast_path", &sta_cfg);
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
        ESP_LOGW(TAG, "no saved wifi profile: STA will not connect (AP-only until configured)");
        return ESP_ERR_NOT_FOUND;
    }

    err = app_wifi_perform_scan();
    if (err != ESP_OK) {
        app_wifi_runtime_lock();
        app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "scan_failed");
        app_wifi_runtime_unlock();
        ESP_LOGW(TAG, "wifi scan failed (%s): cannot select saved AP", esp_err_to_name(err));
        return err;
    }

    for (;;) {
        uint8_t attempted_mask;
        app_wifi_profiles_lock();
        attempted_mask = s_wifi_attempted_mask;
        app_wifi_profiles_unlock();
        profile_index = app_wifi_select_best_profile_index(attempted_mask);
        if (profile_index < 0) {
            uint16_t scanned_snapshot;
            size_t pi;

            app_wifi_profiles_lock();
            s_wifi_attempted_mask = 0;
            app_wifi_profiles_unlock();
            app_wifi_runtime_lock();
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "no_saved_ap");
            scanned_snapshot = s_wifi.scan_count;
            app_wifi_runtime_unlock();
            /* 这是"设备看起来卡死"的最常见真因:一切正常,只是扫描里没有已存的 AP */
            ESP_LOGW(TAG, "no untried saved AP left (profiles=%u, scanned=%u): STA stays down",
                     (unsigned)profile_count, (unsigned)scanned_snapshot);
            /* 1.3-04:把"为什么连不上"变成可自证的一行日志 ——
             * 命中扫描:给出 rssi/auth 与阈值的比较(安全模式不兼容/信号弱一目了然);
             * 未命中:明确说明 AP 不在扫描表里(关机/隐藏/超出距离)。
             * 否则现场只看到 reason=210(NO_AP_FOUND_W_COMPAT_SECURITY),无法判断。 */
            app_wifi_profiles_lock();
            for (pi = 0; pi < s_wifi_profile_count; pi++) {
                const char *wanted = (const char *)s_wifi_profiles[pi].ssid;
                uint16_t si;
                bool seen_any = false;

                for (si = 0; si < scanned_snapshot; si++) {
                    const char *seen_ssid = NULL;
                    int8_t seen_rssi = 0;
                    uint8_t seen_auth = 0;

                    if (!app_wifi_scan_record_at(si, &seen_ssid, &seen_rssi, &seen_auth)) {
                        continue;
                    }
                    if (seen_ssid == NULL || strcmp(seen_ssid, wanted) != 0) {
                        continue;
                    }
                    seen_any = true;
                    {
                        char seen_bssid[18] = "";
                        size_t seen_pwd_len = strlen(s_wifi_profiles[pi].password);
                        uint8_t min_auth = (seen_pwd_len >= 8) ? (uint8_t)APP_WIFI_STA_AUTHMODE_MIN
                                                               : (uint8_t)WIFI_AUTH_OPEN;

                        (void)app_wifi_scan_record_bssid(si, seen_bssid, sizeof(seen_bssid));
                        ESP_LOGW(TAG,
                                 "  profile[%u] '%s' in scan: bssid=%s rssi=%d auth=%u(%s) "
                                 "pwd_len=%u min=%u(%s) -> %s",
                                 (unsigned)pi, wanted, seen_bssid, (int)seen_rssi,
                                 (unsigned)seen_auth, app_wifi_auth_name(seen_auth),
                                 (unsigned)seen_pwd_len, (unsigned)min_auth,
                                 app_wifi_auth_name(min_auth),
                                 (seen_pwd_len == 0)
                                     ? "PASSWORD EMPTY -> STA looks like an OPEN client (210)"
                                     : (seen_auth < min_auth)
                                           ? "SECURITY BELOW THRESHOLD, will not connect"
                                           : "compatible: check password or signal");
                    }
                }
                if (!seen_any) {
                    ESP_LOGW(TAG, "  profile[%u] '%s' NOT in scan (AP off / hidden / out of range?)",
                             (unsigned)pi, wanted);
                }
            }
            app_wifi_profiles_unlock();
            return ESP_ERR_NOT_FOUND;
        }
        err = app_wifi_connect_profile_index(profile_index);
        if (err == ESP_OK) {
            app_wifi_profiles_lock();
            /* pwd_len 用于核对"主配置 vs profile"是否一致(1.3-21:排查口令首字符丢失) */
            ESP_LOGI(TAG, "auto_connect: selected saved profile index=%d ssid=%s pwd_len=%u",
                     profile_index, s_wifi_profiles[profile_index].ssid,
                     (unsigned)strlen(s_wifi_profiles[profile_index].password));
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

/* STA 断连原因码 → 可读名(1.3版本优化.md 1.3-08)。
 * 覆盖外场最常见的失败:NO_AP_FOUND / 4WAY_HANDSHAKE_TIMEOUT(=口令错) / AUTH_FAIL 等。 */
static const char *app_wifi_reason_name(uint8_t reason)
{
    switch (reason) {
    case 1:   return "UNSPECIFIED";
    case 2:   return "AUTH_EXPIRE";
    case 3:   return "AUTH_LEAVE";
    case 4:   return "ASSOC_EXPIRE";
    case 5:   return "ASSOC_TOOMANY";
    case 6:   return "NOT_AUTHED";
    case 7:   return "NOT_ASSOCED";
    case 8:   return "ASSOC_LEAVE";
    case 9:   return "ASSOC_NOT_AUTHED";
    case 15:  return "4WAY_HANDSHAKE_TIMEOUT";
    case 16:  return "GROUP_KEY_UPDATE_TIMEOUT";
    case 17:  return "IE_IN_4WAY_DIFFERS";
    case 200: return "BEACON_TIMEOUT";
    case 201: return "NO_AP_FOUND";
    case 202: return "AUTH_FAIL";
    case 203: return "ASSOC_FAIL";
    case 204: return "HANDSHAKE_TIMEOUT";
    case 205: return "CONNECTION_FAIL";
    case 206: return "AP_TSF_RESET";
    case 207: return "ROAMING";
    case 208: return "ASSOC_COMEBACK_TIMEOUT";
    case 209: return "SA_QUERY_TIMEOUT";
    case 210: return "NO_AP_FOUND_W_COMPAT_SECURITY";   /* 1.3-04 实测命中:安全模式与阈值不兼容 */
    case 211: return "NO_AP_FOUND_IN_AUTHMODE_THRESHOLD";
    case 212: return "NO_AP_FOUND_IN_RSSI_THRESHOLD";
    default:  return "OTHER";
    }
}

/* 扫描所见的 AP 安全模式 → 可读名(1.3-04:让"为什么不兼容"一眼可读) */
static const char *app_wifi_auth_name(uint8_t auth)
{
    switch (auth) {
    case WIFI_AUTH_OPEN:            return "OPEN";
    case WIFI_AUTH_WEP:             return "WEP";
    case WIFI_AUTH_WPA_PSK:         return "WPA";
    case WIFI_AUTH_WPA2_PSK:        return "WPA2";
    case WIFI_AUTH_WPA_WPA2_PSK:    return "WPA/WPA2";
    case WIFI_AUTH_WPA2_ENTERPRISE: return "WPA2-ENT";
    case WIFI_AUTH_WPA3_PSK:        return "WPA3";
    case WIFI_AUTH_WPA2_WPA3_PSK:   return "WPA2/WPA3";
    default:                        return "OTHER";
    }
}

/* 打印"我们实际在要求什么"——排查 210 时与扫描到的 auth 直接对照(1.3-21) */
static void app_wifi_log_sta_cfg(const char *where, const wifi_config_t *cfg)
{
    ESP_LOGI(TAG, "%s: ssid='%s' pwd_len=%u threshold=%u(%s) pmf(capable=%d,required=%d) scan=%u",
             where,
             (const char *)cfg->sta.ssid,
             (unsigned)strlen((const char *)cfg->sta.password),
             (unsigned)cfg->sta.threshold.authmode,
             app_wifi_auth_name((uint8_t)cfg->sta.threshold.authmode),
             (int)cfg->sta.pmf_cfg.capable,
             (int)cfg->sta.pmf_cfg.required,
             (unsigned)cfg->sta.scan_method);
}

/* 210 兜底:AP 要求 PMF(802.11w)时首轮 required=false 会连不上,带 required=true 再试一次 */
static esp_err_t app_wifi_retry_with_pmf_required(void)
{
    wifi_config_t sta_cfg = {0};
    esp_err_t err;

    app_config_lock();
    app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), s_config.sta_ssid);
    app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), s_config.sta_password);
    app_config_unlock();

    if (sta_cfg.sta.ssid[0] == '\0' || sta_cfg.sta.password[0] == '\0') {
        ESP_LOGW(TAG, "PMF retry skipped: empty sta_ssid/sta_password in config");
        return ESP_ERR_INVALID_STATE;
    }
    sta_cfg.sta.threshold.authmode =
        (strlen((const char *)sta_cfg.sta.password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = true;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    app_wifi_log_sta_cfg("pmf_required_retry", &sta_cfg);
    err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "PMF retry set_config failed: %s", esp_err_to_name(err));
        return err;
    }
    return esp_wifi_connect();
}

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_AP_STACONNECTED: {
            wifi_event_ap_staconnected_t *ap_sta = event_data;
            /* AP 客户端加入/离开必须可见:排查"手机连着 AP 但网页转圈/半截"
             * —— 若 STA 抖动带着 AP 换信道,客户端会反复掉线重连,此处会连续刷屏 */
            ESP_LOGI(TAG, "AP client joined: %02x:%02x:%02x:%02x:%02x:%02x aid=%u",
                     ap_sta->mac[0], ap_sta->mac[1], ap_sta->mac[2],
                     ap_sta->mac[3], ap_sta->mac[4], ap_sta->mac[5],
                     (unsigned)ap_sta->aid);
            break;
        }
        case WIFI_EVENT_AP_STADISCONNECTED: {
            wifi_event_ap_stadisconnected_t *ap_sta = event_data;
            ESP_LOGI(TAG, "AP client left: %02x:%02x:%02x:%02x:%02x:%02x aid=%u reason=%u",
                     ap_sta->mac[0], ap_sta->mac[1], ap_sta->mac[2],
                     ap_sta->mac[3], ap_sta->mac[4], ap_sta->mac[5],
                     (unsigned)ap_sta->aid, (unsigned)ap_sta->reason);
            break;
        }
        case WIFI_EVENT_STA_START:
            app_wifi_profiles_lock();
            s_wifi_attempted_mask = 0;
            app_wifi_profiles_unlock();
            app_wifi_start_startup_connect_window();
            app_wifi_schedule_connect_task(0);
            break;
        case WIFI_EVENT_STA_CONNECTED: {
            bool use_static = false;

            /* 试连验证:连接成功即记结果,跳过一切正常运行逻辑 */
            if (s_tryconnect_active && s_tryconnect_connecting) {
                s_tryconnect_result = APP_WIFI_TRY_OK;
                if (s_tryconnect_events != NULL) {
                    xEventGroupSetBits(s_tryconnect_events, APP_WIFI_TRY_DONE_BIT);
                }
                break;
            }

            app_wifi_runtime_lock();
            s_wifi.sta_connected = true;
            s_wifi.sta_retries = 0;
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = false;
            s_wifi_startup_retry_at_ms = app_wifi_now_ms() + APP_WIFI_STA_STARTUP_RETRY_INTERVAL_MS;
            s_sta_connected_at_ms = app_wifi_now_ms();

            app_config_lock();
            use_static = s_config.sta_use_static;
            app_config_unlock();

            if (use_static) {
                /* 静态 IP 已在 apply_wifi_config 时写入 netif,连接后无需再动 DHCP;
                 * IP_EVENT_STA_GOT_IP 仍会用该 IP 触发。 */
                ESP_LOGI(TAG, "STA_CONNECTED: static ip mode (connected at t=%lldms)",
                         (long long)s_sta_connected_at_ms);
            } else {
                esp_err_t stop_err = esp_netif_dhcpc_stop(s_sta_netif);
                if (stop_err != ESP_OK && stop_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
                    ESP_LOGW(TAG, "STA_CONNECTED: dhcpc_stop: %s", esp_err_to_name(stop_err));
                }
                esp_err_t start_err = esp_netif_dhcpc_start(s_sta_netif);
                if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                    ESP_LOGW(TAG, "STA_CONNECTED: dhcpc_start: %s", esp_err_to_name(start_err));
                }
                ESP_LOGI(TAG, "STA_CONNECTED: dhcpc restarted (connected at t=%lldms)",
                         (long long)s_sta_connected_at_ms);
            }
            break;
        }
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = event_data;
            uint8_t sta_retries_snapshot;
            int active_profile_snapshot;
            uint8_t net_mode_snapshot;
            bool need_schedule_connect = false;
            bool do_retry_direct = false;

            /* 试连验证:旧链路拆除的断开事件忽略(s_tryconnect_connecting=false),
             * 候选连接失败才记结果;无论如何都跳过正常重连逻辑 */
            if (s_tryconnect_active) {
                if (s_tryconnect_connecting) {
                    switch (disconnected->reason) {
                    case 15:  /* 4WAY_HANDSHAKE_TIMEOUT:口令错误最常见 */
                    case 202: /* AUTH_FAIL */
                    case 2:   /* AUTH_EXPIRE */
                        s_tryconnect_result = APP_WIFI_TRY_WRONG_PASSWORD;
                        break;
                    case 201: /* NO_AP_FOUND */
                    case 203: /* ASSOC_FAIL */
                        s_tryconnect_result = APP_WIFI_TRY_AP_NOT_FOUND;
                        break;
                    default:
                        s_tryconnect_result = APP_WIFI_TRY_CONNECT_FAILED;
                        break;
                    }
                    ESP_LOGI(TAG, "tryconnect failed: reason=%d(%s)", disconnected->reason,
                             app_wifi_reason_name(disconnected->reason));
                    if (s_tryconnect_events != NULL) {
                    xEventGroupSetBits(s_tryconnect_events, APP_WIFI_TRY_DONE_BIT);
                }
                }
                break;
            }

            app_wifi_runtime_lock();
            s_wifi.sta_connected = false;
            s_wifi.sta_has_ip = false;
            s_wifi.sta_ip_conflict = false;
            sta_retries_snapshot = s_wifi.sta_retries;
            app_wifi_runtime_unlock();
            s_wifi_sta_connecting = false;
            app_set_sta_ip_strings(NULL);
            app_wifi_runtime_lock();
            snprintf(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "%d(%s)",
                     disconnected->reason, app_wifi_reason_name(disconnected->reason));
            app_wifi_runtime_unlock();

            app_config_lock();
            net_mode_snapshot = s_config.net_mode;
            app_config_unlock();
            app_wifi_profiles_lock();
            active_profile_snapshot = s_wifi_active_profile_index;
            app_wifi_profiles_unlock();

            /* 外场可诊断(1.3版本优化.md 1.3-08):断连原因 + 重试计数必须可见。
             * 旧实现只写 last_disconnect 字符串、**不打任何日志**,现场与"设备卡死"无法区分。 */
            ESP_LOGW(TAG, "STA disconnected: reason=%d(%s) retries=%u has_sta=%d profile=%d",
                     disconnected->reason, app_wifi_reason_name(disconnected->reason),
                     (unsigned)sta_retries_snapshot,
                     (int)app_network_mode_has_sta(net_mode_snapshot),
                     active_profile_snapshot);

            /* 1.3-21:210 在"AP 要求 PMF(802.11w)"时也会出现 —— 首轮 required=false 失败后,
             * 带 required=true 再试一次(每次启动只试一次,避免反复翻转)。 */
            if (disconnected->reason == 210 && !s_wifi_pmf_required_tried) {
                s_wifi_pmf_required_tried = true;
                app_wifi_runtime_lock();
                s_wifi.sta_retries = 0;
                app_wifi_runtime_unlock();
                s_wifi_sta_connecting = true;
                ESP_LOGW(TAG, "reason=210: retry once with PMF required (802.11w)");
                if (app_wifi_retry_with_pmf_required() != ESP_OK) {
                    ESP_LOGW(TAG, "PMF-required retry could not start");
                }
                break;
            }

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

        /* 试连验证:拿 IP 属候选连接,不写运行时状态/不回写 profile */
        if (s_tryconnect_active) {
            return;
        }

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

        /* 静态 IP 模式:拿到地址后做一次 ARP 冲突探测(见 app_sta_ip_conflict_check_task) */
        {
            bool use_static = false;

            app_config_lock();
            use_static = s_config.sta_use_static;
            app_config_unlock();
            if (use_static) {
                app_sta_ip_conflict_check_start();
            }
        }

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
            /* 护栏(1.3版本优化.md 1.3-20):profile 的 ssid/口令为空时**不回写**主配置 ——
             * 否则会把可用的凭据用空值覆盖并落盘(自我强化地丢口令)。 */
            if (profile_ssid[0] != '\0' && profile_password[0] != '\0') {
                app_config_lock();
                app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), profile_ssid);
                app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), profile_password);
                app_config_save(&s_config);
                app_config_unlock();
                app_config_save_wifi_profile(profile_ssid, profile_password);
                app_wifi_load_saved_profiles();
            } else {
                ESP_LOGW(TAG, "profile %d has empty ssid/password; skip config writeback",
                         active_profile_snapshot);
            }
        }
    }
}

static esp_err_t app_apply_wifi_config(void)
{
    wifi_config_t ap_cfg = {0};
    wifi_config_t sta_cfg = {0};
    esp_err_t err;
    uint8_t net_mode_snapshot;
    char ap_ssid_log[sizeof(s_config.ap_ssid)];
    char sta_ssid_log[sizeof(s_config.sta_ssid)];
    char sta_pwd_log[sizeof(s_config.sta_password)];   /* 仅用于打印长度,绝不打印内容 */
    bool sta_use_static = false;

    app_config_lock();
    sta_use_static = s_config.sta_use_static;
    if (app_network_mode_has_ap(s_config.net_mode)) {
        app_copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), s_config.ap_ssid);
        ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
        app_copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), s_config.ap_password);
    }
    if (app_network_mode_has_sta(s_config.net_mode)) {
        app_copy_fixed_bytes(sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), s_config.sta_ssid);
        app_copy_fixed_bytes(sta_cfg.sta.password, sizeof(sta_cfg.sta.password), s_config.sta_password);
    }
    app_copy_string(ap_ssid_log, sizeof(ap_ssid_log), s_config.ap_ssid);
    app_copy_string(sta_ssid_log, sizeof(sta_ssid_log), s_config.sta_ssid);
    app_copy_string(sta_pwd_log, sizeof(sta_pwd_log), s_config.sta_password);
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
            (strlen((const char *)sta_cfg.sta.password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
        sta_cfg.sta.pmf_cfg.capable = true;
        sta_cfg.sta.pmf_cfg.required = false;
        sta_cfg.sta.scan_method = WIFI_FAST_SCAN;
    }
    if (app_network_mode_has_sta(net_mode_snapshot)) {
        app_wifi_log_sta_cfg("apply_wifi_config", &sta_cfg);
    }

    /* 打印"实际存储值"而不是"是否启用":此前非 STA 模式下恒打 <disabled>,
     * 会把"SSID 已存但模式未启用"(1.3-04)掩盖成"没保存",排障时严重误导 */
    ESP_LOGI(TAG, "apply_wifi_config: net_mode=%d has_ap=%d has_sta=%d ap_ssid='%s' sta_ssid='%s' sta_pwd_len=%u",
             net_mode_snapshot,
             (int)app_network_mode_has_ap(net_mode_snapshot),
             (int)app_network_mode_has_sta(net_mode_snapshot),
             ap_ssid_log[0] != '\0' ? ap_ssid_log : "<empty>",
             sta_ssid_log[0] != '\0' ? sta_ssid_log : "<empty>",
             (unsigned)strlen(sta_pwd_log));

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    app_wifi_runtime_lock();
    s_wifi.sta_connected = false;
    s_wifi.sta_has_ip = false;
    s_wifi.sta_ip_conflict = false;
    s_wifi.sta_retries = 0;
    s_wifi.startup_fallback_to_ap = false;
    app_wifi_runtime_unlock();
    s_wifi_sta_connecting = false;
    s_wifi_pmf_required_tried = false;   /* 重新配置网络后允许再次 PMF 兜底 */
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
        /* 1.3-69:AP 的 netif IP 不能在这里配 —— 此时 esp_wifi_start() 还没调用,
         * AP 接口未就绪,set_ip_info 不生效/会被随后的 start 重置成 IDF 默认
         * 192.168.4.1(状态接口因此报 .4.1,而设计值是 192.168.8.1)。
         * IP/DHCP 选项统一移到 esp_wifi_start() 成功之后。 */
    }
    if (app_network_mode_has_sta(net_mode_snapshot)) {
        err = esp_wifi_set_config(WIFI_IF_STA, &sta_cfg);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply_wifi_config: set sta config failed: %s", esp_err_to_name(err));
            return err;
        }
        if (sta_use_static) {
            err = app_configure_sta_static_ip();
            if (err != ESP_OK) {
                ESP_LOGE(TAG, "apply_wifi_config: static ip config failed: %s", esp_err_to_name(err));
                return err;
            }
        } else {
            /* DHCP 模式:如之前被静态 IP 停止,恢复 DHCP 客户端 */
            esp_err_t start_err = esp_netif_dhcpc_start(s_sta_netif);
            if (start_err != ESP_OK && start_err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STARTED) {
                ESP_LOGW(TAG, "apply_wifi_config: dhcpc_start: %s", esp_err_to_name(start_err));
            }
        }
    }

    ESP_LOGI(TAG, "apply_wifi_config: calling esp_wifi_start...");
    err = esp_wifi_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "apply_wifi_config: start failed: %s", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "apply_wifi_config: esp_wifi_start returned OK");

    /* 1.3-69:AP 接口起来后再设 IP/DHCP 选项。先 stop dhcps → set_ip_info →
     * DNS/captive URI → start dhcps(见 app_configure_ap_netif_ip)。失败只告警:
     * AP 退回 IDF 默认 192.168.4.1 仍可用,不把整次 WiFi 启动判失败。 */
    if (app_network_mode_has_ap(net_mode_snapshot)) {
        err = app_configure_ap_netif_ip();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "apply_wifi_config: post-start ap netif ip failed: %s", esp_err_to_name(err));
        }
    }
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

/* ---------------------------------------------------------------------------
 * 试连验证(1.3-37):临时换候选 SSID/口令真连一次验证口令正确性。
 * 期间事件处理器只记结果(见 s_tryconnect_active 分支),结束无条件恢复旧配置,
 * 再交回常规自动重连路径回到原状态。调用方(HTTP handler)会阻塞等待结果。
 * ------------------------------------------------------------------------- */
app_wifi_try_result_t app_wifi_try_connect(const char *ssid, const char *password,
                                           uint32_t timeout_ms)
{
    wifi_config_t old_cfg = {0};
    wifi_config_t candidate = {0};
    uint16_t scan_count;
    bool ssid_seen = false;
    EventGroupHandle_t events;
    esp_err_t err;
    int i;

    if (ssid == NULL || ssid[0] == '\0' || strlen(ssid) > 32) {
        return APP_WIFI_TRY_INVALID_ARG;
    }
    if (password == NULL) {
        password = "";
    }
    if (s_tryconnect_active || s_wifi_connect_task != NULL) {
        ESP_LOGW(TAG, "tryconnect busy (active=%d connect_task=%p)",
                 (int)s_tryconnect_active, (void *)s_wifi_connect_task);
        return APP_WIFI_TRY_BUSY;
    }

    /* 预检(方案 C):扫描确认 SSID 在场。隐藏网络扫不到 —— 属预期限制,前端文案说明 */
    if (app_wifi_perform_scan() == ESP_OK) {
        app_wifi_runtime_lock();
        scan_count = s_wifi.scan_count;
        app_wifi_runtime_unlock();
        for (i = 0; i < (int)scan_count; i++) {
            const char *seen = NULL;
            int8_t rssi = 0;
            uint8_t auth = 0;

            if (app_wifi_scan_record_at((uint16_t)i, &seen, &rssi, &auth) &&
                seen != NULL && strcmp(seen, ssid) == 0) {
                ssid_seen = true;
                break;
            }
        }
        if (!ssid_seen) {
            ESP_LOGW(TAG, "tryconnect: ssid '%s' not in scan results", ssid);
            return APP_WIFI_TRY_AP_NOT_FOUND;
        }
    }

    if (esp_wifi_get_config(WIFI_IF_STA, &old_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "tryconnect: get sta config failed");
        return APP_WIFI_TRY_CONNECT_FAILED;
    }

    app_copy_fixed_bytes(candidate.sta.ssid, sizeof(candidate.sta.ssid), ssid);
    app_copy_fixed_bytes(candidate.sta.password, sizeof(candidate.sta.password), password);
    candidate.sta.threshold.authmode =
        (strlen(password) >= 8) ? APP_WIFI_STA_AUTHMODE_MIN : WIFI_AUTH_OPEN;
    candidate.sta.pmf_cfg.capable = true;
    candidate.sta.pmf_cfg.required = false;
    candidate.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    {
        EventGroupHandle_t created;

        portENTER_CRITICAL(&s_tryconnect_mux);
        created = s_tryconnect_events;
        if (created == NULL) {
            created = xEventGroupCreateStatic(&s_tryconnect_events_buf);
            s_tryconnect_events = created;
        }
        portEXIT_CRITICAL(&s_tryconnect_mux);
        events = created;
    }
    if (events == NULL) {
        return APP_WIFI_TRY_CONNECT_FAILED;
    }

    xEventGroupClearBits(events, APP_WIFI_TRY_DONE_BIT);
    s_tryconnect_result = APP_WIFI_TRY_TIMEOUT;
    s_tryconnect_active = true;
    s_tryconnect_connecting = false;
    s_wifi_sta_connecting = true;   /* 抑制 boot monitor 的自动调度 */

    esp_wifi_disconnect();
    err = esp_wifi_set_config(WIFI_IF_STA, &candidate);
    if (err == ESP_OK) {
        s_tryconnect_connecting = true;
        app_wifi_log_sta_cfg("tryconnect", &candidate);
        err = esp_wifi_connect();
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "tryconnect: set_config/connect failed: %s", esp_err_to_name(err));
        s_tryconnect_active = false;
        s_tryconnect_connecting = false;
        s_wifi_sta_connecting = false;
        (void)esp_wifi_set_config(WIFI_IF_STA, &old_cfg);
        app_wifi_schedule_connect_task(pdMS_TO_TICKS(400));
        return APP_WIFI_TRY_CONNECT_FAILED;
    }

    if (xEventGroupWaitBits(events, APP_WIFI_TRY_DONE_BIT, pdTRUE, pdFALSE,
                            pdMS_TO_TICKS(timeout_ms)) == 0) {
        s_tryconnect_result = APP_WIFI_TRY_TIMEOUT;
    }

    /* 无条件恢复:断开候选连接,放回旧配置,交回常规自动重连 */
    esp_wifi_disconnect();
    s_tryconnect_active = false;
    s_tryconnect_connecting = false;
    s_wifi_sta_connecting = false;
    if (esp_wifi_set_config(WIFI_IF_STA, &old_cfg) != ESP_OK) {
        ESP_LOGW(TAG, "tryconnect: restore sta config failed");
    }
    app_wifi_schedule_connect_task(pdMS_TO_TICKS(400));

    ESP_LOGI(TAG, "tryconnect ssid='%s' result=%d", ssid, (int)s_tryconnect_result);
    return s_tryconnect_result;
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

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();

    /* hostname 与 storage 属非致命配置,失败降级告警,不中止启动 */
    {
        esp_err_t hostname_err = esp_netif_set_hostname(s_ap_netif, hostname);
        if (hostname_err != ESP_OK) {
            ESP_LOGW(TAG, "set ap hostname failed: %s", esp_err_to_name(hostname_err));
        }
        hostname_err = esp_netif_set_hostname(s_sta_netif, hostname);
        if (hostname_err != ESP_OK) {
            ESP_LOGW(TAG, "set sta hostname failed: %s", esp_err_to_name(hostname_err));
        }
    }

    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    {
        esp_err_t storage_err = esp_wifi_set_storage(WIFI_STORAGE_RAM);
        if (storage_err != ESP_OK) {
            ESP_LOGW(TAG, "set wifi storage RAM failed: %s", esp_err_to_name(storage_err));
        }
    }
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL));
    app_wifi_mark_profiles_dirty();
    if (s_wifi_boot_monitor_task == NULL) {
        if (xTaskCreate(app_wifi_boot_monitor_task_fn, "wifi_boot_monitor", 4096,
                        NULL, 5, &s_wifi_boot_monitor_task) != pdPASS) {
            s_wifi_boot_monitor_task = NULL;
            ESP_LOGE(TAG, "Failed to create wifi_boot_monitor task "
                          "(startup STA-failure AP fallback will be unavailable)");
        }
    }
    ESP_ERROR_CHECK(app_apply_wifi_config());
}
