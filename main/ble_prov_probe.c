#include "sdkconfig.h"

#include "ble_prov_probe.h"

#if CONFIG_IOTHUB_BLE_PROV_PROBE

#include <stdio.h>
#include <string.h>

#include "config.h"
#include "constants.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"
#include "nvs.h"
#include "prov_pin.h"

static const char *TAG = "ble_prov_probe";

#define PROBE_NVS_NAMESPACE "ble_prov"
#define PROBE_NVS_DONE_KEY  "done"
#define PROBE_SERVICE_PREFIX "PROV_IOT_"

extern app_config_t s_config;

static char s_ssid[33];
static char s_password[65];
static bool s_have_creds;
static bool s_committing;
static esp_timer_handle_t s_reboot_timer;

static bool app_ble_prov_probe_done(void)
{
    nvs_handle_t handle;
    uint8_t done = 0;

    if (nvs_open(PROBE_NVS_NAMESPACE, NVS_READONLY, &handle) != ESP_OK) {
        return false;
    }
    esp_err_t err = nvs_get_u8(handle, PROBE_NVS_DONE_KEY, &done);
    nvs_close(handle);
    return err == ESP_OK && done != 0;
}

static esp_err_t app_ble_prov_probe_mark_done(void)
{
    nvs_handle_t handle;
    esp_err_t err;

    err = nvs_open(PROBE_NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_u8(handle, PROBE_NVS_DONE_KEY, 1);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}

static void app_ble_prov_copy_wifi_field(char *dst, size_t dst_size,
                                         const uint8_t *src, size_t src_size)
{
    size_t len;

    if (dst_size == 0) {
        return;
    }
    len = strnlen((const char *)src, src_size);
    if (len >= dst_size) {
        len = dst_size - 1;
    }
    memcpy(dst, src, len);
    dst[len] = '\0';
}

static esp_err_t app_ble_prov_save_config(void)
{
    app_config_t cfg;
    esp_err_t err;

    app_config_lock();
    cfg = s_config;
    app_config_unlock();

    snprintf(cfg.sta_ssid, sizeof(cfg.sta_ssid), "%s", s_ssid);
    snprintf(cfg.sta_password, sizeof(cfg.sta_password), "%s", s_password);
    cfg.net_mode = NET_MODE_STA;     /* 与现有低内存运行基线一致;APSTA 会多占约 12KB */
    cfg.sta_use_static = 0;          /* 配网结果按 DHCP 保存 */

    err = app_config_save(&cfg);
    if (err == ESP_OK) {
        app_config_lock();
        s_config = cfg;
        app_config_unlock();
        (void)app_config_save_wifi_profile(cfg.sta_ssid, cfg.sta_password);
    }
    return err;
}

static void app_ble_prov_log_heap(const char *stage)
{
    ESP_LOGI(TAG, "%s: free=%u largest=%u internal_free=%u dma_free=%u",
             stage,
             (unsigned)esp_get_free_heap_size(),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_8BIT | MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_DMA));
}

static void app_ble_prov_log_tasks(const char *stage)
{
    TaskStatus_t tasks[24];
    UBaseType_t count;
    UBaseType_t i;

    count = uxTaskGetSystemState(tasks, sizeof(tasks) / sizeof(tasks[0]), NULL);
    for (i = 0; i < count; i++) {
        ESP_LOGI(TAG, "%s task=%s hwm_words=%u",
                 stage, tasks[i].pcTaskName, (unsigned)tasks[i].usStackHighWaterMark);
    }
}

static void app_ble_prov_reboot_cb(void *arg)
{
    (void)arg;
    esp_restart();
}

static void app_ble_prov_event_handler(void *arg, esp_event_base_t event_base,
                                       int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == NETWORK_PROV_EVENT) {
        switch (event_id) {
        case NETWORK_PROV_START:
            ESP_LOGI(TAG, "provisioning started");
            app_ble_prov_log_heap("prov_start");
            break;
        case NETWORK_PROV_WIFI_CRED_RECV: {
            wifi_sta_config_t *sta = (wifi_sta_config_t *)event_data;

            app_ble_prov_copy_wifi_field(s_ssid, sizeof(s_ssid),
                                         sta->ssid, sizeof(sta->ssid));
            app_ble_prov_copy_wifi_field(s_password, sizeof(s_password),
                                         sta->password, sizeof(sta->password));
            s_have_creds = true;
            ESP_LOGI(TAG, "wifi credentials received (ssid=%s)", s_ssid);
            break;
        }
        case NETWORK_PROV_WIFI_CRED_FAIL: {
            network_prov_wifi_sta_fail_reason_t *reason =
                (network_prov_wifi_sta_fail_reason_t *)event_data;

            ESP_LOGW(TAG, "wifi credential failed: %s",
                     (*reason == NETWORK_PROV_WIFI_STA_AUTH_ERROR)
                         ? "auth_error" : "ap_not_found");
            (void)network_prov_mgr_reset_wifi_sm_state_on_failure();
            app_ble_prov_log_heap("prov_fail");
            break;
        }
        case NETWORK_PROV_WIFI_CRED_SUCCESS:
            ESP_LOGI(TAG, "wifi credentials accepted");
            break;
        case NETWORK_PROV_END:
            ESP_LOGI(TAG, "provisioning ended");
            break;
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *ip = (ip_event_got_ip_t *)event_data;

        ESP_LOGI(TAG, "sta got ip: " IPSTR, IP2STR(&ip->ip_info.ip));
        app_ble_prov_log_heap("got_ip");
        if (!s_have_creds || s_committing) {
            return;
        }
        s_committing = true;
        esp_err_t err = ESP_FAIL;

        for (int i = 0; i < 3; i++) {
            err = app_ble_prov_save_config();
            if (err == ESP_OK) {
                break;
            }
            ESP_LOGE(TAG, "config save failed (%s), retry %d/3",
                     esp_err_to_name(err), i + 1);
            vTaskDelay(pdMS_TO_TICKS(200));
        }
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "probe commit failed; not rebooting");
            s_committing = false;
            return;
        }
        err = app_ble_prov_probe_mark_done();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "done marker failed: %s; not rebooting", esp_err_to_name(err));
            s_committing = false;
            return;
        }
        ESP_LOGI(TAG, "credentials committed; rebooting in 10s");
        app_ble_prov_log_heap("before_reboot");
        app_ble_prov_log_tasks("before_reboot");
        /* 给官方 App 足够时间收到最终响应并主动断开,避免设备提前重启被误报失败 */
        esp_err_t timer_err = esp_timer_start_once(s_reboot_timer, 10ULL * 1000 * 1000);
        if (timer_err != ESP_OK) {
            ESP_LOGE(TAG, "reboot timer failed: %s; rebooting in 3s", esp_err_to_name(timer_err));
            vTaskDelay(pdMS_TO_TICKS(3000));
            esp_restart();
        }
    }
}

bool app_ble_prov_probe_should_start(void)
{
    return !app_ble_prov_probe_done();
}

void app_ble_prov_probe_run(void)
{
    char service_name[16];
    char pop[APP_PROV_PIN_MAX_LEN + 1];
    uint8_t mac[6];

    ESP_LOGI(TAG, "probe mode: official provisioning owns Wi-Fi/NimBLE");
    app_ble_prov_log_heap("boot");

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    (void)esp_netif_create_default_wifi_sta();

    wifi_init_config_t wifi_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_cfg));
    (void)esp_wifi_set_storage(WIFI_STORAGE_RAM);

    ESP_ERROR_CHECK(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                               &app_ble_prov_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                               &app_ble_prov_event_handler, NULL));

    network_prov_mgr_config_t mgr_cfg = {
        .scheme = network_prov_scheme_ble,
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    ESP_ERROR_CHECK(network_prov_mgr_init(mgr_cfg));
    ESP_ERROR_CHECK(network_prov_mgr_disable_auto_stop(1000));

    const esp_timer_create_args_t reboot_timer_args = {
        .callback = app_ble_prov_reboot_cb,
        .name = "prov_reboot",
    };
    ESP_ERROR_CHECK(esp_timer_create(&reboot_timer_args, &s_reboot_timer));

    if (app_prov_pin_get(pop, sizeof(pop)) != ESP_OK) {
        snprintf(pop, sizeof(pop), "%s", APP_PROV_PIN_DEFAULT);
        ESP_LOGW(TAG, "provisioning PIN read failed; using default");
    }

    esp_read_mac(mac, ESP_MAC_BT);
    snprintf(service_name, sizeof(service_name), PROBE_SERVICE_PREFIX "%02X%02X",
             mac[4], mac[5]);

    ESP_LOGI(TAG, "starting BLE provisioning: service=%s security=1 pin_source=nvs/default",
             service_name);
    ESP_ERROR_CHECK(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                                        (const void *)pop,
                                                        service_name, NULL));
    app_ble_prov_log_heap("prov_running");
    app_ble_prov_log_tasks("prov_running");
}

#endif /* CONFIG_IOTHUB_BLE_PROV_PROBE */
