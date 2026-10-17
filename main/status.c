#include <stdio.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_heap_caps.h"
#include "lwip/ip4_addr.h"

#include "bt.h"
#include "bt_gattc.h"
#include "bt_gatts.h"
#include "common.h"
#include "config.h"
#include "data_pipe.h"
#include "http_priv.h"
#include "http_utils.h"
#include "mqtt.h"
#include "ota.h"
#include "state.h"
#include "status.h"
#include "uart.h"
#include "wifi.h"

extern app_config_t s_config;

void app_status_get_ap_network_strings(char *ip, size_t ip_len, char *gateway, size_t gw_len,
                                       char *netmask, size_t netmask_len)
{
    esp_netif_ip_info_t ip_info = {0};
    bool ap_enabled = false;

    app_config_lock();
    ap_enabled = app_network_mode_has_ap(s_config.net_mode);
    app_config_unlock();

    if (!ap_enabled || s_ap_netif == NULL || esp_netif_get_ip_info(s_ap_netif, &ip_info) != ESP_OK) {
        app_copy_string(ip, ip_len, "192.168.8.1");
        app_copy_string(gateway, gw_len, "192.168.8.1");
        app_copy_string(netmask, netmask_len, "255.255.255.0");
        return;
    }
    snprintf(ip, ip_len, IPSTR, IP2STR(&ip_info.ip));
    snprintf(gateway, gw_len, IPSTR, IP2STR(&ip_info.gw));
    snprintf(netmask, netmask_len, IPSTR, IP2STR(&ip_info.netmask));
}

/* `send_cjson_response()` 已随 1.3版本优化.md 1.3-12 移除:两个热接口改为
 * app_http_send_jsonf() 零堆直出,不再需要"cJSON 建树 + 序列化"的两份堆。 */

esp_err_t app_status_send_device_info(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    char mac_str[18];
    esp_chip_info_t chip_info;
    const esp_app_desc_t *app_desc = esp_app_get_description();

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    esp_chip_info(&chip_info);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    /* 1.3-12:零堆直出。字段名/顺序/格式与改造前**逐字节一致**(基线 /tmp/base_device_info.json) */
    return app_http_send_jsonf(req, NULL,
                               "{\"project\":\"iothub\",\"idf_target\":\"%s\","
                               "\"idf_version\":\"%s\",\"app_version\":\"%s\","
                               "\"compile_date\":\"%s\",\"compile_time\":\"%s\","
                               "\"mac\":\"%s\",\"cpu_cores\":%d,\"free_heap\":%u}",
                               CONFIG_IDF_TARGET, esp_get_idf_version(), app_desc->version,
                               app_desc->date, app_desc->time, mac_str,
                               (int)chip_info.cores, (unsigned)esp_get_free_heap_size());
}

esp_err_t app_status_send_device_status(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    char mac_str[18];
    char frame[8];
    char ip[16];
    char gateway[16];
    char netmask[16];
    size_t app_total = 0;
    size_t app_used = 0;
    size_t storage_total = 0;
    size_t storage_used = 0;
    esp_chip_info_t chip_info;
    const esp_app_desc_t *app_desc = esp_app_get_description();
    const esp_partition_t *running_partition = NULL;
    esp_err_t rv;
    data_pipe_stats_t dp_stats;

    uint8_t s_uart_data_bits;
    uart_parity_t s_uart_parity_mode;
    uint8_t s_uart_stop_bits;
    uint32_t s_uart_baudrate;
    uint8_t s_net_mode;
    char s_ap_ssid[sizeof(s_config.ap_ssid)];
    char s_sta_ssid[sizeof(s_config.sta_ssid)];
    uint8_t s_relay_on;
    uint8_t s_led_on;
    uint8_t s_bt_mode;
    char s_bt_device_name[sizeof(s_config.bt_device_name)];
    char s_mqtt_backend[sizeof(s_config.mqtt_backend)];
    char s_mqtt_host[sizeof(s_config.mqtt_host)];
    uint16_t s_mqtt_port;
    uint8_t s_mqtt_use_tls;
    uint8_t s_bt_runtime_snapshot;
    char s_bt_last_error_snapshot[64];

    bool w_sta_connected;
    bool w_sta_has_ip;
    bool w_startup_connect_active;
    bool w_startup_fallback_to_ap;
    bool w_sta_ip_conflict;
    int64_t w_startup_connect_deadline_ms;
    char w_sta_ip[sizeof(s_wifi.sta_ip)];
    char w_startup_fallback_reason[sizeof(s_wifi.startup_fallback_reason)];

    bool m_connected;
    uint32_t m_publish_count;
    char m_last_error[sizeof(s_mqtt.last_error)];

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    esp_chip_info(&chip_info);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    app_config_lock();
    s_uart_data_bits = s_config.uart_data_bits;
    s_uart_parity_mode = (uart_parity_t)s_config.uart_parity_mode;
    s_uart_stop_bits = s_config.uart_stop_bits;
    s_uart_baudrate = s_config.uart_baudrate;
    s_net_mode = s_config.net_mode;
    memcpy(s_ap_ssid, s_config.ap_ssid, sizeof(s_ap_ssid));
    memcpy(s_sta_ssid, s_config.sta_ssid, sizeof(s_sta_ssid));
    s_relay_on = s_config.relay_on;
    s_led_on = s_config.led_on;
    s_bt_mode = s_config.bt_role;
    memcpy(s_bt_device_name, s_config.bt_device_name, sizeof(s_bt_device_name));
    memcpy(s_mqtt_backend, s_config.mqtt_backend, sizeof(s_mqtt_backend));
    memcpy(s_mqtt_host, s_config.mqtt_host, sizeof(s_mqtt_host));
    s_mqtt_port = s_config.mqtt_port;
    s_mqtt_use_tls = s_config.mqtt_use_tls;
    app_config_unlock();

    app_wifi_runtime_lock();
    w_sta_connected = s_wifi.sta_connected;
    w_sta_has_ip = s_wifi.sta_has_ip;
    w_startup_connect_active = s_wifi.startup_connect_active;
    w_startup_fallback_to_ap = s_wifi.startup_fallback_to_ap;
    w_sta_ip_conflict = s_wifi.sta_ip_conflict;
    w_startup_connect_deadline_ms = s_wifi.startup_connect_deadline_ms;
    memcpy(w_sta_ip, s_wifi.sta_ip, sizeof(w_sta_ip));
    memcpy(w_startup_fallback_reason, s_wifi.startup_fallback_reason, sizeof(w_startup_fallback_reason));
    app_wifi_runtime_unlock();

    app_mqtt_lock();
    m_connected = s_mqtt.connected;
    m_publish_count = s_mqtt.publish_count;
    memcpy(m_last_error, s_mqtt.last_error, sizeof(m_last_error));
    app_mqtt_unlock();

    app_bt_get_status(&s_bt_runtime_snapshot, s_bt_last_error_snapshot, sizeof(s_bt_last_error_snapshot));

    app_uart_build_frame(frame, sizeof(frame),
                         s_uart_data_bits,
                         s_uart_parity_mode,
                         s_uart_stop_bits);
    app_status_get_ap_network_strings(ip, sizeof(ip), gateway, sizeof(gateway), netmask, sizeof(netmask));
    app_ota_get_running_partition_stats(&running_partition, &app_total, &app_used);
    app_ota_get_storage_stats(&storage_total, &storage_used);

    /* 1.3-12:零堆直出 —— 字段名/顺序/格式与改造前**逐字节一致**(基线 /tmp/base_device_status.json)。
     * app_json_escaped() 的轮转槽只在 app_json_build_lock() 保护下使用,故并发请求不会互相覆盖。 */
    data_pipe_get_stats(&dp_stats);
    app_json_build_lock();
    rv = app_http_send_jsonf(req, NULL,
        "{\"project\":\"iothub\",\"idf_target\":\"%s\",\"idf_version\":\"%s\",\"app_version\":\"%s\""
        ",\"mac\":\"%s\",\"cpu_cores\":%d,\"free_heap\":%u,\"largest_free\":%u,\"httpd_clients\":%u,\"httpd_max_clients\":%u"
        ",\"app_partition\":{\"label\":\"%s\",\"total_bytes\":%u,\"used_bytes\":%u,\"free_bytes\":%u}"
        ",\"storage\":{\"label\":\"storage\",\"total_bytes\":%u,\"used_bytes\":%u,\"free_bytes\":%u}"
        ",\"network\":{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\",\"gateway\":\"%s\""
        ",\"netmask\":\"%s\",\"sta_ssid\":\"%s\",\"sta_connected\":%s,\"sta_has_ip\":%s"
        ",\"sta_ip\":\"%s\",\"startup_connect_active\":%s,\"startup_connect_deadline_ms\":%lld"
        ",\"startup_fallback_to_ap\":%s,\"startup_fallback_reason\":\"%s\",\"sta_ip_conflict\":%s}"
        ",\"gpio\":{\"relay_gpio\":%d,\"led_gpio\":%d,\"input_gpio\":%d,\"relay_on\":%s,\"led_on\":%s"
        ",\"input_active\":%s}"
        ",\"bluetooth\":{\"mode\":\"%s\",\"device_name\":\"%s\",\"runtime_mode\":\"%s\",\"adv_active\":%s,\"connected\":%s,\"last_error\":\"%s\"}"
        ",\"mqtt\":{\"backend\":\"%s\",\"host\":\"%s\",\"port\":%u,\"use_tls\":%s,\"connected\":%s"
        ",\"last_error\":\"%s\",\"publish_count\":%u,\"outbox\":%d}"
        ",\"uart\":{\"rx_gpio\":%d,\"tx_gpio\":%d,\"baudrate\":%u,\"frame\":\"%s\"}"
        ",\"data_pipe\":{\"enabled\":%s,\"loopback\":%s,\"rx_uart1\":%u,\"rx_ble\":%u,\"rx_mqtt\":%u"
        ",\"tx_uart1\":%u,\"tx_ble\":%u,\"tx_mqtt\":%u"
        ",\"dropped\":%u,\"overrun\":%u"
        ",\"dropped_uart1\":%u,\"dropped_ble\":%u,\"dropped_mqtt\":%u"
        ",\"overrun_uart1\":%u,\"overrun_ble\":%u,\"overrun_mqtt\":%u"
        ",\"mqtt_port_ready\":%s,\"queue_depth\":%u,\"queue_high_water\":%u}}",
        CONFIG_IDF_TARGET, esp_get_idf_version(), app_desc->version, mac_str,
        (int)chip_info.cores, (unsigned)esp_get_free_heap_size(), (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT),
        (unsigned)app_http_client_count(),
        (unsigned)app_http_client_max(),
        running_partition != NULL ? running_partition->label : "unknown",
        (unsigned)app_total, (unsigned)app_used,
        (unsigned)(app_total > app_used ? (app_total - app_used) : 0),
        (unsigned)storage_total, (unsigned)storage_used,
        (unsigned)(storage_total > storage_used ? (storage_total - storage_used) : 0),
        app_network_mode_to_string(s_net_mode),
        app_json_escaped(s_ap_ssid), ip, gateway, netmask, app_json_escaped(s_sta_ssid),
        w_sta_connected ? "true" : "false", w_sta_has_ip ? "true" : "false", w_sta_ip,
        w_startup_connect_active ? "true" : "false", (long long)w_startup_connect_deadline_ms,
        w_startup_fallback_to_ap ? "true" : "false", app_json_escaped(w_startup_fallback_reason),
        w_sta_ip_conflict ? "true" : "false",
        (int)app_relay_gpio(), (int)app_led_gpio(), (int)app_input_gpio(),
        s_relay_on ? "true" : "false", s_led_on ? "true" : "false",
        app_get_input_state() ? "true" : "false",
        app_bt_mode_to_string(s_bt_mode), app_json_escaped(s_bt_device_name),
        app_bt_mode_to_string(s_bt_runtime_snapshot),
        app_bt_advertising() ? "true" : "false",
        /* 1.3-60: central(GATTC)连接也要计入,否则主角色下状态永远报 connected=false */
        (app_ble_gatts_connected() || app_ble_gattc_connected()) ? "true" : "false",
        app_json_escaped(s_bt_last_error_snapshot),
        app_json_escaped(s_mqtt_backend), app_json_escaped(s_mqtt_host), (unsigned)s_mqtt_port,
        s_mqtt_use_tls ? "true" : "false", m_connected ? "true" : "false",
        app_json_escaped(m_last_error), (unsigned)m_publish_count, app_mqtt_outbox_size(),
        (int)app_uart_rx_gpio(), (int)app_uart_tx_gpio(), (unsigned)s_uart_baudrate, frame,
        data_pipe_enabled() ? "true" : "false", data_pipe_loopback() ? "true" : "false",
        (unsigned)dp_stats.rx[DATA_PORT_UART1], (unsigned)dp_stats.rx[DATA_PORT_BLE],
        (unsigned)dp_stats.rx[DATA_PORT_MQTT],
        (unsigned)dp_stats.tx[DATA_PORT_UART1], (unsigned)dp_stats.tx[DATA_PORT_BLE],
        (unsigned)dp_stats.tx[DATA_PORT_MQTT],
        (unsigned)(dp_stats.dropped[DATA_PORT_UART1] + dp_stats.dropped[DATA_PORT_BLE] +
                   dp_stats.dropped[DATA_PORT_MQTT]),
        (unsigned)(dp_stats.overrun[DATA_PORT_UART1] + dp_stats.overrun[DATA_PORT_BLE] +
                   dp_stats.overrun[DATA_PORT_MQTT]),
        (unsigned)dp_stats.dropped[DATA_PORT_UART1], (unsigned)dp_stats.dropped[DATA_PORT_BLE],
        (unsigned)dp_stats.dropped[DATA_PORT_MQTT],
        (unsigned)dp_stats.overrun[DATA_PORT_UART1], (unsigned)dp_stats.overrun[DATA_PORT_BLE],
        (unsigned)dp_stats.overrun[DATA_PORT_MQTT],
        (data_pipe_port_registered(DATA_PORT_MQTT) && m_connected) ? "true" : "false",
        (unsigned)dp_stats.queue_depth, (unsigned)dp_stats.queue_high_water);
    app_json_build_unlock();
    return rv;
}
