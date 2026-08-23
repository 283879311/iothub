#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_mac.h"

#include "common.h"
#include "config.h"
#include "constants.h"
#include "identity.h"

static const char *APP_HOSTNAME_PREFIX = "IotHub-";
static const char *APP_BT_BLE_PREFIX = "IotHubBLE-";
static const char *APP_BT_SPP_PREFIX = "IotHubSPP-";
static char s_app_hostname[16] = "";
static char s_app_ap_ssid[33] = "";
static char s_app_bt_ble_name[33] = "";
static char s_app_bt_spp_name[33] = "";
static bool s_app_identity_ready = false;

extern app_config_t s_config;

static void app_init_identity_names(void)
{
    uint8_t mac[6];

    if (s_app_identity_ready) {
        return;
    }

    ESP_ERROR_CHECK(esp_read_mac(mac, ESP_MAC_WIFI_STA));
    snprintf(s_app_hostname, sizeof(s_app_hostname), "%s%02X%02X",
             APP_HOSTNAME_PREFIX, mac[4], mac[5]);
    snprintf(s_app_ap_ssid, sizeof(s_app_ap_ssid), "%s%02X%02X",
             APP_HOSTNAME_PREFIX, mac[4], mac[5]);
    snprintf(s_app_bt_ble_name, sizeof(s_app_bt_ble_name), "%s%02X%02X",
             APP_BT_BLE_PREFIX, mac[4], mac[5]);
    snprintf(s_app_bt_spp_name, sizeof(s_app_bt_spp_name), "%s%02X%02X",
             APP_BT_SPP_PREFIX, mac[4], mac[5]);
    s_app_identity_ready = true;
}

const char *app_get_hostname(void)
{
    app_init_identity_names();
    return s_app_hostname;
}

static bool app_ap_ssid_is_managed(const char *ssid)
{
    app_init_identity_names();

    return ssid == NULL ||
           ssid[0] == '\0' ||
           strcmp(ssid, "iothub") == 0 ||
           strcmp(ssid, "IotHub") == 0 ||
           strcmp(ssid, s_app_ap_ssid) == 0 ||
           strncmp(ssid, APP_HOSTNAME_PREFIX, strlen(APP_HOSTNAME_PREFIX)) == 0;
}

bool app_bt_device_name_is_managed(const char *name)
{
    app_init_identity_names();

    return name == NULL ||
           name[0] == '\0' ||
           strcmp(name, "iothub-bt") == 0 ||
           strcmp(name, s_app_bt_ble_name) == 0 ||
           strcmp(name, s_app_bt_spp_name) == 0 ||
           strncmp(name, APP_BT_BLE_PREFIX, strlen(APP_BT_BLE_PREFIX)) == 0 ||
           strncmp(name, APP_BT_SPP_PREFIX, strlen(APP_BT_SPP_PREFIX)) == 0;
}

static const char *app_get_managed_bt_device_name(uint8_t mode)
{
    app_init_identity_names();
    return mode == BT_MODE_SPP ? s_app_bt_spp_name : s_app_bt_ble_name;
}

void app_set_managed_bt_device_name(uint8_t mode)
{
    app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name),
                    app_get_managed_bt_device_name(mode));
}

void app_refresh_managed_identity_fields(void)
{
    if (app_ap_ssid_is_managed(s_config.ap_ssid)) {
        app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), s_app_ap_ssid);
    }
    if (app_bt_device_name_is_managed(s_config.bt_device_name)) {
        app_set_managed_bt_device_name(s_config.bt_mode);
    }
}
