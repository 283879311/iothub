#pragma once

#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"
#include "constants.h"

typedef struct {
    char ssid[33];
    char password[65];
} app_wifi_profile_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t bt_mode;
    uint8_t net_mode;
    uint8_t mqtt_use_tls;
    uint8_t uart_parity_mode;
    uint8_t uart_data_bits;
    uint8_t uart_stop_bits;
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_t;

void app_config_set_defaults(app_config_t *config);
esp_err_t app_config_save(const app_config_t *config);
void app_config_load(app_config_t *config);
esp_err_t app_config_load_wifi_profiles(app_wifi_profile_t *profiles, size_t max_profiles, size_t *profile_count);
esp_err_t app_config_save_wifi_profile(const char *ssid, const char *password);
esp_err_t app_config_delete_wifi_profile(const char *ssid);

void app_config_lock(void);
void app_config_unlock(void);
void app_wifi_runtime_lock(void);
void app_wifi_runtime_unlock(void);
void app_wifi_profiles_lock(void);
void app_wifi_profiles_unlock(void);
