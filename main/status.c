#include <stdio.h>

#include "cJSON.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "lwip/ip4_addr.h"

#include "bt.h"
#include "common.h"
#include "config.h"
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

static esp_err_t send_cjson_response(httpd_req_t *req, cJSON *root)
{
    char *serialized;
    esp_err_t err;

    if (root == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    serialized = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (serialized == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    err = app_http_send_json_text(req, NULL, serialized);
    cJSON_free(serialized);
    return err;
}

esp_err_t app_status_send_device_info(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    char mac_str[18];
    esp_chip_info_t chip_info;
    const esp_app_desc_t *app_desc = esp_app_get_description();
    cJSON *root;

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    esp_chip_info(&chip_info);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    root = cJSON_CreateObject();
    if (root == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    cJSON_AddStringToObject(root, "project", "iothub");
    cJSON_AddStringToObject(root, "idf_target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(root, "app_version", app_desc->version);
    cJSON_AddStringToObject(root, "compile_date", app_desc->date);
    cJSON_AddStringToObject(root, "compile_time", app_desc->time);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddNumberToObject(root, "cpu_cores", (double)chip_info.cores);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());
    return send_cjson_response(req, root);
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
    cJSON *root, *net, *gpio, *bt, *mqtt_obj, *uart, *app_part, *storage;

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
    s_bt_mode = s_config.bt_mode;
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
    w_startup_connect_deadline_ms = s_wifi.startup_connect_deadline_ms;
    memcpy(w_sta_ip, s_wifi.sta_ip, sizeof(w_sta_ip));
    memcpy(w_startup_fallback_reason, s_wifi.startup_fallback_reason, sizeof(w_startup_fallback_reason));
    app_wifi_runtime_unlock();

    app_mqtt_lock();
    m_connected = s_mqtt.connected;
    m_publish_count = s_mqtt.publish_count;
    memcpy(m_last_error, s_mqtt.last_error, sizeof(m_last_error));
    app_mqtt_unlock();

    s_bt_runtime_snapshot = s_bt_runtime_mode;
    memcpy(s_bt_last_error_snapshot, s_bt_last_error, sizeof(s_bt_last_error_snapshot));

    app_uart_build_frame(frame, sizeof(frame),
                         s_uart_data_bits,
                         s_uart_parity_mode,
                         s_uart_stop_bits);
    app_status_get_ap_network_strings(ip, sizeof(ip), gateway, sizeof(gateway), netmask, sizeof(netmask));
    app_ota_get_running_partition_stats(&running_partition, &app_total, &app_used);
    app_ota_get_storage_stats(&storage_total, &storage_used);

    root = cJSON_CreateObject();
    if (root == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }
    cJSON_AddStringToObject(root, "project", "iothub");
    cJSON_AddStringToObject(root, "idf_target", CONFIG_IDF_TARGET);
    cJSON_AddStringToObject(root, "idf_version", esp_get_idf_version());
    cJSON_AddStringToObject(root, "app_version", app_desc->version);
    cJSON_AddStringToObject(root, "mac", mac_str);
    cJSON_AddNumberToObject(root, "cpu_cores", (double)chip_info.cores);
    cJSON_AddNumberToObject(root, "free_heap", (double)esp_get_free_heap_size());

    app_part = cJSON_AddObjectToObject(root, "app_partition");
    if (app_part != NULL) {
        cJSON_AddStringToObject(app_part, "label",
                                running_partition != NULL ? running_partition->label : "unknown");
        cJSON_AddNumberToObject(app_part, "total_bytes", (double)app_total);
        cJSON_AddNumberToObject(app_part, "used_bytes", (double)app_used);
        cJSON_AddNumberToObject(app_part, "free_bytes",
                                (double)(app_total > app_used ? (app_total - app_used) : 0));
    }
    storage = cJSON_AddObjectToObject(root, "storage");
    if (storage != NULL) {
        cJSON_AddStringToObject(storage, "label", "storage");
        cJSON_AddNumberToObject(storage, "total_bytes", (double)storage_total);
        cJSON_AddNumberToObject(storage, "used_bytes", (double)storage_used);
        cJSON_AddNumberToObject(storage, "free_bytes",
                                (double)(storage_total > storage_used ? (storage_total - storage_used) : 0));
    }

    net = cJSON_AddObjectToObject(root, "network");
    if (net != NULL) {
        cJSON_AddStringToObject(net, "mode", app_network_mode_to_string(s_net_mode));
        cJSON_AddStringToObject(net, "ap_ssid", s_ap_ssid);
        cJSON_AddStringToObject(net, "ap_ip", ip);
        cJSON_AddStringToObject(net, "gateway", gateway);
        cJSON_AddStringToObject(net, "netmask", netmask);
        cJSON_AddStringToObject(net, "sta_ssid", s_sta_ssid);
        cJSON_AddBoolToObject(net, "sta_connected", w_sta_connected);
        cJSON_AddBoolToObject(net, "sta_has_ip", w_sta_has_ip);
        cJSON_AddStringToObject(net, "sta_ip", w_sta_ip);
        cJSON_AddBoolToObject(net, "startup_connect_active", w_startup_connect_active);
        cJSON_AddNumberToObject(net, "startup_connect_deadline_ms", (double)w_startup_connect_deadline_ms);
        cJSON_AddBoolToObject(net, "startup_fallback_to_ap", w_startup_fallback_to_ap);
        cJSON_AddStringToObject(net, "startup_fallback_reason", w_startup_fallback_reason);
    }

    gpio = cJSON_AddObjectToObject(root, "gpio");
    if (gpio != NULL) {
        cJSON_AddNumberToObject(gpio, "relay_gpio", (double)app_relay_gpio());
        cJSON_AddNumberToObject(gpio, "led_gpio", (double)app_led_gpio());
        cJSON_AddNumberToObject(gpio, "input_gpio", (double)app_input_gpio());
        cJSON_AddBoolToObject(gpio, "relay_on", s_relay_on);
        cJSON_AddBoolToObject(gpio, "led_on", s_led_on);
        cJSON_AddBoolToObject(gpio, "input_active", app_get_input_state());
    }

    bt = cJSON_AddObjectToObject(root, "bluetooth");
    if (bt != NULL) {
        cJSON_AddStringToObject(bt, "mode", app_bt_mode_to_string(s_bt_mode));
        cJSON_AddStringToObject(bt, "device_name", s_bt_device_name);
        cJSON_AddStringToObject(bt, "runtime_mode", app_bt_mode_to_string(s_bt_runtime_snapshot));
        cJSON_AddStringToObject(bt, "last_error", s_bt_last_error_snapshot);
    }

    mqtt_obj = cJSON_AddObjectToObject(root, "mqtt");
    if (mqtt_obj != NULL) {
        cJSON_AddStringToObject(mqtt_obj, "backend", s_mqtt_backend);
        cJSON_AddStringToObject(mqtt_obj, "host", s_mqtt_host);
        cJSON_AddNumberToObject(mqtt_obj, "port", (double)s_mqtt_port);
        cJSON_AddBoolToObject(mqtt_obj, "use_tls", s_mqtt_use_tls);
        cJSON_AddBoolToObject(mqtt_obj, "connected", m_connected);
        cJSON_AddStringToObject(mqtt_obj, "last_error", m_last_error);
        cJSON_AddNumberToObject(mqtt_obj, "publish_count", (double)m_publish_count);
    }

    uart = cJSON_AddObjectToObject(root, "uart");
    if (uart != NULL) {
        cJSON_AddNumberToObject(uart, "rx_gpio", (double)app_uart_rx_gpio());
        cJSON_AddNumberToObject(uart, "tx_gpio", (double)app_uart_tx_gpio());
        cJSON_AddNumberToObject(uart, "baudrate", (double)s_uart_baudrate);
        cJSON_AddStringToObject(uart, "frame", frame);
    }
    return send_cjson_response(req, root);
}
