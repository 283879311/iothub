#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "runtime.h"

extern esp_netif_t *s_ap_netif;
extern wifi_runtime_t s_wifi;

const char *app_network_mode_to_string(uint8_t mode);
uint8_t app_network_mode_from_string(const char *mode);
bool app_network_mode_has_sta(uint8_t mode);
bool app_network_mode_has_ap(uint8_t mode);
esp_err_t app_wifi_perform_scan(void);
void app_wifi_restart_task(void *arg);
bool app_wifi_schedule_restart(void);
void app_start_wifi(void);

bool app_wifi_scan_record_at(uint16_t index,
                             const char **ssid_out,
                             int8_t *rssi_out,
                             uint8_t *authmode_out);

void app_wifi_mark_profiles_dirty(void);
void app_wifi_sta_config_updated(void);
