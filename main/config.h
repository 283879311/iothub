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
    /* v7 新增,追加在末尾:v6 blob 是 v7 的字节前缀,迁移按前缀保留 */
    char web_username[33];
    uint8_t web_pass_salt[16];
    uint8_t web_pass_hash[32];
    /* v8 新增,追加在末尾(同上:v7 blob 是 v8 的字节前缀):
     * 蓝牙角色(权威字段) —— 0=off / 1=peripheral(从) / 2=central(主)。
     * 旧 v7 的 bt_mode 保留为兼容字段(0=off,1=ble),迁移时用于推导 bt_role。 */
    uint8_t bt_role;

    /* v9 新增:STA 静态 IP 配置(追加在末尾,v8 blob 是 v9 的字节前缀) */
    uint8_t sta_use_static;        /* 0=DHCP,1=静态 IP */
    char sta_static_ip[16];
    char sta_static_gateway[16];
    char sta_static_netmask[16];
    char sta_static_dns1[16];
    char sta_static_dns2[16];

    /* v10 新增(1.3-07):数据面三边路由位图(每端口的目的掩码,见 data_pipe.h)。
     * 默认:UART1→BLE、BLE→UART1|MQTT(上行)、MQTT→BLE(下行)。 */
    uint32_t data_route_uart1;
    uint32_t data_route_ble;
    uint32_t data_route_mqtt;
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
