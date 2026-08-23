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
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
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
    cJSON *root, *net, *gpio, *bt, *mqtt, *uart, *app_part, *storage;

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    esp_chip_info(&chip_info);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    app_uart_build_frame(frame, sizeof(frame),
                         s_config.uart_data_bits,
                         (uart_parity_t)s_config.uart_parity_mode,
                         s_config.uart_stop_bits);
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
        cJSON_AddStringToObject(net, "mode", app_network_mode_to_string(s_config.net_mode));
        cJSON_AddStringToObject(net, "ap_ssid", s_config.ap_ssid);
        cJSON_AddStringToObject(net, "ap_ip", ip);
        cJSON_AddStringToObject(net, "gateway", gateway);
        cJSON_AddStringToObject(net, "netmask", netmask);
        cJSON_AddStringToObject(net, "sta_ssid", s_config.sta_ssid);
        cJSON_AddBoolToObject(net, "sta_connected", s_wifi.sta_connected);
        cJSON_AddBoolToObject(net, "sta_has_ip", s_wifi.sta_has_ip);
        cJSON_AddStringToObject(net, "sta_ip", s_wifi.sta_ip);
        cJSON_AddBoolToObject(net, "startup_connect_active", s_wifi.startup_connect_active);
        cJSON_AddNumberToObject(net, "startup_connect_deadline_ms", (double)s_wifi.startup_connect_deadline_ms);
        cJSON_AddBoolToObject(net, "startup_fallback_to_ap", s_wifi.startup_fallback_to_ap);
        cJSON_AddStringToObject(net, "startup_fallback_reason", s_wifi.startup_fallback_reason);
    }

    gpio = cJSON_AddObjectToObject(root, "gpio");
    if (gpio != NULL) {
        cJSON_AddNumberToObject(gpio, "relay_gpio", (double)app_relay_gpio());
        cJSON_AddNumberToObject(gpio, "led_gpio", (double)app_led_gpio());
        cJSON_AddNumberToObject(gpio, "input_gpio", (double)app_input_gpio());
        cJSON_AddBoolToObject(gpio, "relay_on", s_config.relay_on);
        cJSON_AddBoolToObject(gpio, "led_on", s_config.led_on);
        cJSON_AddBoolToObject(gpio, "input_active", app_get_input_state());
    }

    bt = cJSON_AddObjectToObject(root, "bluetooth");
    if (bt != NULL) {
        cJSON_AddStringToObject(bt, "mode", app_bt_mode_to_string(s_config.bt_mode));
        cJSON_AddStringToObject(bt, "device_name", s_config.bt_device_name);
        cJSON_AddStringToObject(bt, "runtime_mode", app_bt_mode_to_string(s_bt_runtime_mode));
    }

    mqtt = cJSON_AddObjectToObject(root, "mqtt");
    if (mqtt != NULL) {
        cJSON_AddStringToObject(mqtt, "backend", s_config.mqtt_backend);
        cJSON_AddStringToObject(mqtt, "host", s_config.mqtt_host);
        cJSON_AddNumberToObject(mqtt, "port", (double)s_config.mqtt_port);
        cJSON_AddBoolToObject(mqtt, "use_tls", s_config.mqtt_use_tls);
        cJSON_AddBoolToObject(mqtt, "connected", s_mqtt.connected);
        cJSON_AddStringToObject(mqtt, "last_error", s_mqtt.last_error);
        cJSON_AddNumberToObject(mqtt, "publish_count", (double)s_mqtt.publish_count);
    }

    uart = cJSON_AddObjectToObject(root, "uart");
    if (uart != NULL) {
        cJSON_AddNumberToObject(uart, "rx_gpio", (double)app_uart_rx_gpio());
        cJSON_AddNumberToObject(uart, "tx_gpio", (double)app_uart_tx_gpio());
        cJSON_AddNumberToObject(uart, "baudrate", (double)s_config.uart_baudrate);
        cJSON_AddStringToObject(uart, "frame", frame);
    }
    return send_cjson_response(req, root);
}
