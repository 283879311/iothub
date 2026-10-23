#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "esp_netif.h"

#include "runtime.h"

extern wifi_runtime_t s_wifi;

const char *app_network_mode_to_string(uint8_t mode);
uint8_t app_network_mode_from_string(const char *mode);
bool app_network_mode_has_sta(uint8_t mode);
esp_err_t app_wifi_perform_scan(void);
/* BLE/串口维护命令扫描:暂停自动重连、等待当前 connect task、独占扫描。 */
esp_err_t app_wifi_perform_scan_command(void);
void app_wifi_restart_task(void *arg);
bool app_wifi_schedule_restart(void);
void app_start_wifi(void);

bool app_wifi_scan_record_at(uint16_t index,
                             const char **ssid_out,
                             int8_t *rssi_out,
                             uint8_t *authmode_out);

/* 扫描记录里的 BSSID(格式 aa:bb:cc:dd:ee:ff)——用于判定"是不是同一台 AP"(1.3-21) */
bool app_wifi_scan_record_bssid(uint16_t index, char *out, size_t out_size);

void app_wifi_mark_profiles_dirty(void);
void app_wifi_sta_config_updated(void);

/* 试连验证(1.3-37):临时换候选 SSID/口令真连一次,出结果后无条件恢复旧配置 */
typedef enum {
    APP_WIFI_TRY_OK = 0,          /* STA_CONNECTED:口令正确 */
    APP_WIFI_TRY_WRONG_PASSWORD,  /* reason 15/202/2:4WAY 握手/认证失败 */
    APP_WIFI_TRY_AP_NOT_FOUND,    /* 扫描未见该 SSID,或 reason 201/203 */
    APP_WIFI_TRY_CONNECT_FAILED,  /* 其余失败 */
    APP_WIFI_TRY_TIMEOUT,         /* 等待超时 */
    APP_WIFI_TRY_BUSY,            /* 自动重连/试连进行中 */
    APP_WIFI_TRY_CANCELLED,       /* 操作取消 */
    APP_WIFI_TRY_INVALID_ARG,
} app_wifi_try_result_t;

app_wifi_try_result_t app_wifi_try_connect(const char *ssid, const char *password,
                                           uint32_t timeout_ms);
/* G2 候选：必须等待候选获得 IP 才返回 APP_WIFI_TRY_OK。 */
app_wifi_try_result_t app_wifi_try_connect_ip(const char *ssid, const char *password,
                                              uint32_t timeout_ms);
/* 请求取消正在进行的 tryconnect；true 表示当时有活动试连。 */
bool app_wifi_try_cancel(void);
