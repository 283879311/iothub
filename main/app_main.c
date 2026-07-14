#include <ctype.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#include "driver/gpio.h"
#include "driver/rmt_rx.h"
#include "driver/uart.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"
#include "esp_bt.h"
#include "esp_bt_defs.h"
#include "esp_bt_device.h"
#include "esp_bt_main.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_gap_ble_api.h"
#include "esp_gap_bt_api.h"
#include "esp_gatts_api.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_spp_api.h"
#include "esp_system.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/ip4_addr.h"
#include "mqtt_client.h"
#include "nvs.h"
#include "nvs_flash.h"

#define APP_NAMESPACE "iothub"
#define APP_CONFIG_KEY "runtime_cfg"
#define APP_CONFIG_MAGIC 0x494f5448UL
#define APP_CONFIG_VERSION 6
#define APP_BASE_PATH "/web"
#define APP_SCRATCH_SIZE 4096
#define APP_JSON_BUFFER_SIZE 4096
#define APP_OTA_REBOOT_DELAY_MS 1500

// Relay output GPIO. Recommended choices are output-capable pins such as
// GPIO18/19/21/22/23/25/26/27/32/33. Avoid Flash/PSRAM pins and role conflicts.
#define RELAY_DEFAULT_GPIO GPIO_NUM_16
// Status LED GPIO. Use a dedicated output-capable pin and avoid input-only pins.
#define LED_DEFAULT_GPIO GPIO_NUM_25
// Digital input GPIO. GPIO34/35/36/39 are also valid, but require external pull resistors.
#define INPUT_DEFAULT_GPIO GPIO_NUM_33
#define UART_PORT UART_NUM_1
// UART RX GPIO. RX may use GPIO4/16/17/18/19/21/22/23/25/26/27/32/33/34/35/36/39.
#define UART_RX_GPIO GPIO_NUM_26
// UART TX GPIO. TX must use an output-capable pin and must not use GPIO34/35/36/39.
#define UART_TX_GPIO GPIO_NUM_27
#define UART_PROBE_TIME_MS 1200
#define UART_PARITY_PROBE_TIME_MS 250
#define UART_LOW_BAUD_THRESHOLD 4800
#define UART_LOW_BAUD_PROBE_TIME_MS 1000
#define UART_RX_BUFFER_SIZE 2048
#define UART_SAMPLE_BUFFER_SIZE 256
#define UART_CONSOLE_PLAIN_SIZE 1024
#define UART_CONSOLE_HEX_SIZE 3072
#define UART_TX_BUFFER_SIZE 256
#define UART_PROBE_RESULT_MAX 6
#define UART_MANUAL_CANDIDATE_MAX 24
#define WIFI_SCAN_LIST_SIZE 8

#define BT_MODE_OFF 0
#define BT_MODE_BLE 1
#define BT_MODE_SPP 2

#define NET_MODE_AP 0
#define NET_MODE_STA 1
#define NET_MODE_APSTA 2

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
} app_config_v5_t;

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
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    int32_t uart_rx_gpio;
    int32_t uart_tx_gpio;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_v3_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    int32_t relay_gpio;
    int32_t led_gpio;
    int32_t input_gpio;
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t bt_mode;
    uint8_t net_mode;
    uint8_t mqtt_use_tls;
    uint8_t uart_parity_mode;
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    int32_t uart_rx_gpio;
    int32_t uart_tx_gpio;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_v4_t;

typedef struct {
    httpd_handle_t server;
    char scratch[APP_SCRATCH_SIZE];
} web_context_t;

typedef struct {
    esp_mqtt_client_handle_t client;
    bool connected;
    uint32_t publish_count;
    int last_msg_id;
    char last_error[96];
    char uri[128];
} mqtt_runtime_t;

typedef struct {
    uint32_t baudrate;
    uart_parity_t parity;
    int score;
    size_t sample_bytes;
    int ascii_ratio_pct;
    char protocol_hint[24];
    char sample[UART_SAMPLE_BUFFER_SIZE + 1];
} uart_probe_result_t;

typedef struct {
    uint32_t baudrate;
    uart_parity_t parity;
} uart_probe_candidate_t;

// 探测会话使用的临时 UART 帧配置。完全独立于 saved config：
// 自动探测固定 8N1；手动轮询固定 8x1 + 各种 parity。
typedef struct {
    uint32_t baudrate;
    uart_parity_t parity;
    uint8_t data_bits;
    uint8_t stop_bits;
} uart_session_config_t;

typedef struct {
    bool driver_installed;
    bool probe_reliable;
    bool probe_pending_confirmation;
    bool probe_confirmed;
    bool manual_session_active;
    uint32_t detected_baudrate;
    uint32_t raw_baudrate;
    uint32_t edge_count;
    uint32_t low_period;
    uint32_t high_period;
    uint32_t pos_period;
    uint32_t neg_period;
    uint32_t clk_freq_hz;
    uart_parity_t detected_parity;
    size_t last_bytes;
    int ascii_ratio_pct;
    uint32_t rx_sequence;
    int probe_best_score;
    int probe_confirm_score;
    size_t probe_result_count;
    size_t manual_candidate_count;
    size_t manual_candidate_index;
    uint32_t pending_baudrate;
    uint8_t detected_data_bits;
    uint8_t detected_stop_bits;
    uint8_t pending_data_bits;
    uint8_t pending_stop_bits;
    uart_parity_t pending_parity;
    char probe_source[16];
    char probe_mode[16];
    char last_apply_error[48];
    char protocol_hint[24];
    char last_ascii[UART_SAMPLE_BUFFER_SIZE + 1];
    char rx_plain[UART_CONSOLE_PLAIN_SIZE];
    char rx_hex[UART_CONSOLE_HEX_SIZE];
    uart_probe_result_t probe_results[UART_PROBE_RESULT_MAX];
    uart_probe_candidate_t manual_candidates[UART_MANUAL_CANDIDATE_MAX];
    // 探测期间临时使用的帧配置（与 saved config 完全独立）。
    // 自动探测固定 8N1，手动轮询固定 8x1 + parity。
    uart_session_config_t probe_session;
    // 进入探测前对当时运行配置的快照，用于 stop/失败 时恢复。
    // valid=false 表示当前没有挂起的快照。
    uart_session_config_t restore_snapshot;
    bool restore_snapshot_valid;
} uart_runtime_t;

typedef struct {
    bool sta_connected;
    bool sta_has_ip;
    uint8_t sta_retries;
    wifi_ap_record_t scan_records[WIFI_SCAN_LIST_SIZE];
    uint16_t scan_count;
    char sta_ip[16];
    char sta_gateway[16];
    char sta_netmask[16];
    char last_disconnect[32];
} wifi_runtime_t;

static const char *TAG = "iothub";
static const char *SPP_SERVER_NAME = "IOTHUB_SPP";

static app_config_t s_config;
static esp_netif_t *s_ap_netif;
static esp_netif_t *s_sta_netif;
static web_context_t s_web;
static uint8_t s_bt_runtime_mode = BT_MODE_OFF;
static bool s_ble_adv_configured = false;
static bool s_spp_ready = false;
static esp_bd_addr_t s_spp_remote_bda = {0};
static char s_bt_last_error[64] = "";
static mqtt_runtime_t s_mqtt = {0};
static uart_runtime_t s_uart = {0};
// 保护 s_uart.rx_plain / s_uart.rx_hex / s_uart.last_ascii / s_uart.rx_sequence
// 多任务并发写。uart_console_get_handler（1Hz）、
// uart_get_handler（3Hz 状态轮询）、app_mqtt_publish_state（10s）
// 都会进入 app_uart_update_from_raw → app_uart_append_console → app_append_tail，
// 这条链路非原子，需要互斥保护。
static SemaphoreHandle_t s_uart_console_mutex = NULL;

// RMT 探测：单次探测会话的 RMT RX 通道、symbol buffer、信号量。
// 探测过程中创建，结束（成功/失败/超时）后立即删除，确保 RMT 资源不长期占用。
// buffer 大小必须与 mem_block_symbols 匹配：ESP32 非 DMA 模式单 block 最多 64 symbols。
// 64 symbols @ 1us tick ≈ 32 bit 数据，足够解析常见 UART 帧。
static rmt_channel_handle_t s_rmt_probe_channel = NULL;
static rmt_symbol_word_t s_rmt_probe_symbols[64];
static size_t s_rmt_probe_symbol_count = 0;
static SemaphoreHandle_t s_rmt_probe_done_sem = NULL;
static wifi_runtime_t s_wifi = {0};

static void app_uart_reset_probe_metrics(void);
static void app_uart_reset_manual_session(void);
static void app_uart_capture_sample(TickType_t wait_ticks);
static esp_err_t app_uart_install_driver(uint32_t baudrate, uint8_t data_bits,
                                         uart_parity_t parity, uint8_t stop_bits);
static void app_mqtt_stop(void);
static void app_mqtt_restart_task(void *arg);

static esp_ble_adv_params_t s_ble_adv_params = {
    .adv_int_min = 0x20,
    .adv_int_max = 0x40,
    .adv_type = ADV_TYPE_IND,
    .own_addr_type = BLE_ADDR_TYPE_PUBLIC,
    .channel_map = ADV_CHNL_ALL,
    .adv_filter_policy = ADV_FILTER_ALLOW_SCAN_ANY_CON_ANY,
};

static esp_ble_adv_data_t s_ble_adv_data = {
    .set_scan_rsp = false,
    .include_name = true,
    .include_txpower = true,
    .min_interval = 0x20,
    .max_interval = 0x40,
    .appearance = 0x00,
    .manufacturer_len = 0,
    .p_manufacturer_data = NULL,
    .service_data_len = 0,
    .p_service_data = NULL,
    .service_uuid_len = 0,
    .p_service_uuid = NULL,
    .flag = ESP_BLE_ADV_FLAG_GEN_DISC | ESP_BLE_ADV_FLAG_BREDR_NOT_SPT,
};

static void app_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t index = 0;

    if (dst_size == 0) {
        return;
    }

    while (src[index] != '\0' && index + 1 < dst_size) {
        dst[index] = src[index];
        index++;
    }
    dst[index] = '\0';
}

static void app_uart_set_apply_error(const char *error_key)
{
    app_copy_string(s_uart.last_apply_error, sizeof(s_uart.last_apply_error),
                    error_key != NULL ? error_key : "uart_apply_failed");
}

static const char *app_uart_get_apply_error(void)
{
    return s_uart.last_apply_error[0] != '\0' ? s_uart.last_apply_error : "uart_apply_failed";
}

static void app_json_escape_string(char *dst, size_t dst_size, const char *src)
{
    size_t index = 0;

    if (dst_size == 0) {
        return;
    }

    while (*src != '\0' && index + 1 < dst_size) {
        const char *replacement = NULL;
        char ch = *src++;

        switch (ch) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        default:
            break;
        }

        if (replacement != NULL) {
            while (*replacement != '\0' && index + 1 < dst_size) {
                dst[index++] = *replacement++;
            }
            continue;
        }

        if ((unsigned char)ch < 32) {
            dst[index++] = '?';
            continue;
        }

        dst[index++] = ch;
    }

    dst[index] = '\0';
}

static const char *app_bt_mode_to_string(uint8_t mode)
{
    switch (mode) {
    case BT_MODE_BLE:
        return "ble";
    case BT_MODE_SPP:
        return "spp";
    default:
        return "off";
    }
}

static uint8_t app_bt_mode_from_string(const char *mode)
{
    if (strcmp(mode, "ble") == 0) {
        return BT_MODE_BLE;
    }
    if (strcmp(mode, "spp") == 0) {
        return BT_MODE_SPP;
    }
    return BT_MODE_OFF;
}

static const char *app_network_mode_to_string(uint8_t mode)
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

static uint8_t app_network_mode_from_string(const char *mode)
{
    if (strcmp(mode, "sta") == 0) {
        return NET_MODE_STA;
    }
    if (strcmp(mode, "apsta") == 0) {
        return NET_MODE_APSTA;
    }
    return NET_MODE_AP;
}

static const char *app_uart_parity_to_string(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_EVEN:
        return "even";
    case UART_PARITY_ODD:
        return "odd";
    case UART_PARITY_DISABLE:
    default:
        return "none";
    }
}

static const char *app_uart_parity_to_frame(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_EVEN:
        return "8E1";
    case UART_PARITY_ODD:
        return "8O1";
    case UART_PARITY_DISABLE:
    default:
        return "8N1";
    }
}

static bool app_uart_data_bits_valid(uint8_t data_bits)
{
    return data_bits >= 5 && data_bits <= 8;
}

static bool app_uart_stop_bits_valid(uint8_t stop_bits)
{
    return stop_bits == 1 || stop_bits == 2;
}

static uart_word_length_t app_uart_data_bits_to_enum(uint8_t data_bits)
{
    switch (data_bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_stop_bits_t app_uart_stop_bits_to_enum(uint8_t stop_bits)
{
    return stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static void app_uart_build_frame(char *buffer, size_t buffer_size,
                                 uint8_t data_bits, uart_parity_t parity, uint8_t stop_bits)
{
    char parity_char = 'N';

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (parity) {
    case UART_PARITY_EVEN:
        parity_char = 'E';
        break;
    case UART_PARITY_ODD:
        parity_char = 'O';
        break;
    case UART_PARITY_DISABLE:
    default:
        parity_char = 'N';
        break;
    }

    snprintf(buffer, buffer_size, "%u%c%u", (unsigned)data_bits, parity_char, (unsigned)stop_bits);
}

static uart_parity_t app_uart_parity_from_string(const char *value)
{
    if (strcmp(value, "even") == 0 || strcmp(value, "8E1") == 0) {
        return UART_PARITY_EVEN;
    }
    if (strcmp(value, "odd") == 0 || strcmp(value, "8O1") == 0) {
        return UART_PARITY_ODD;
    }
    return UART_PARITY_DISABLE;
}

static gpio_num_t app_relay_gpio(void)
{
    return RELAY_DEFAULT_GPIO;
}

static gpio_num_t app_led_gpio(void)
{
    return LED_DEFAULT_GPIO;
}

static gpio_num_t app_input_gpio(void)
{
    return INPUT_DEFAULT_GPIO;
}

static gpio_num_t app_uart_rx_gpio(void)
{
    return UART_RX_GPIO;
}

static gpio_num_t app_uart_tx_gpio(void)
{
    return UART_TX_GPIO;
}

static bool app_validate_fixed_gpio_config(const char **error_key)
{
    const int32_t relay_gpio = app_relay_gpio();
    const int32_t led_gpio = app_led_gpio();
    const int32_t input_gpio = app_input_gpio();
    const int32_t uart_rx_gpio = app_uart_rx_gpio();
    const int32_t uart_tx_gpio = app_uart_tx_gpio();
    const char *error = NULL;

    switch (relay_gpio) {
    case 4:
    case 16:
    case 17:
    case 18:
    case 19:
    case 21:
    case 22:
    case 23:
    case 25:
    case 26:
    case 27:
    case 32:
    case 33:
        break;
    default:
        error = "invalid_relay_gpio";
        break;
    }

    if (error == NULL) {
        switch (led_gpio) {
        case 4:
        case 16:
        case 17:
        case 18:
        case 19:
        case 21:
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 32:
        case 33:
            break;
        default:
            error = "invalid_led_gpio";
            break;
        }
    }

    if (error == NULL) {
        switch (input_gpio) {
        case 4:
        case 18:
        case 19:
        case 21:
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 39:
            break;
        default:
            error = "invalid_input_gpio";
            break;
        }
    }

    if (error == NULL) {
        switch (uart_rx_gpio) {
        case 4:
        case 16:
        case 17:
        case 18:
        case 19:
        case 21:
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 32:
        case 33:
        case 34:
        case 35:
        case 36:
        case 39:
            break;
        default:
            error = "invalid_uart_rx_gpio";
            break;
        }
    }

    if (error == NULL) {
        switch (uart_tx_gpio) {
        case 4:
        case 16:
        case 17:
        case 18:
        case 19:
        case 21:
        case 22:
        case 23:
        case 25:
        case 26:
        case 27:
        case 32:
        case 33:
            break;
        default:
            error = "invalid_uart_tx_gpio";
            break;
        }
    }

    if (error == NULL && (relay_gpio == led_gpio || relay_gpio == input_gpio || led_gpio == input_gpio)) {
        error = "gpio_pin_conflict";
    }
    if (error == NULL && uart_rx_gpio == uart_tx_gpio) {
        error = "uart_gpio_conflict";
    }
    if (error == NULL &&
        (uart_rx_gpio == relay_gpio || uart_rx_gpio == led_gpio || uart_rx_gpio == input_gpio ||
         uart_tx_gpio == relay_gpio || uart_tx_gpio == led_gpio || uart_tx_gpio == input_gpio)) {
        error = "gpio_uart_conflict";
    }

    if (error_key != NULL) {
        *error_key = error;
    }
    return error == NULL;
}

static void app_sanitize_gpio_config(void)
{
    const char *error = NULL;

    if (app_validate_fixed_gpio_config(&error)) {
        return;
    }

    ESP_LOGW(TAG, "Invalid fixed GPIO config detected (%s)", error != NULL ? error : "unknown");
}

static void app_set_defaults(void)
{
    memset(&s_config, 0, sizeof(s_config));
    s_config.magic = APP_CONFIG_MAGIC;
    s_config.version = APP_CONFIG_VERSION;
    s_config.relay_active_high = 1;
    s_config.led_active_high = 1;
    s_config.input_active_low = 1;
    s_config.bt_mode = BT_MODE_OFF;
    s_config.net_mode = NET_MODE_AP;
    s_config.mqtt_port = 1883;
    s_config.uart_parity_mode = UART_PARITY_DISABLE;
    s_config.uart_data_bits = 8;
    s_config.uart_stop_bits = 1;
    s_config.uart_baudrate = 9600;
    app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), "iothub");
    app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), "iothub-bt");
    app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), "tb_cloud");
    app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), "demo.thingsboard.io");
    app_uart_reset_probe_metrics();
    app_copy_string(s_uart.protocol_hint, sizeof(s_uart.protocol_hint), "unknown");
    app_copy_string(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), "0.0.0.0");
    app_copy_string(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), "0.0.0.0");
    app_copy_string(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), "0.0.0.0");
    app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
}

static esp_err_t app_save_config(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err = nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs_handle, APP_CONFIG_KEY, &s_config, sizeof(s_config));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

static void app_migrate_config_v3(const app_config_v3_t *legacy)
{
    app_set_defaults();
    s_config.relay_active_high = legacy->relay_active_high;
    s_config.led_active_high = legacy->led_active_high;
    s_config.input_active_low = legacy->input_active_low;
    s_config.relay_on = legacy->relay_on;
    s_config.led_on = legacy->led_on;
    s_config.bt_mode = legacy->bt_mode;
    s_config.net_mode = legacy->net_mode;
    s_config.mqtt_use_tls = legacy->mqtt_use_tls;
    s_config.uart_parity_mode = legacy->uart_parity_mode;
    s_config.uart_data_bits = 8;
    s_config.uart_stop_bits = 1;
    s_config.mqtt_port = legacy->mqtt_port;
    s_config.uart_baudrate = legacy->uart_baudrate;
    app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), legacy->ap_ssid);
    app_copy_string(s_config.ap_password, sizeof(s_config.ap_password), legacy->ap_password);
    app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), legacy->sta_ssid);
    app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), legacy->sta_password);
    app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), legacy->bt_device_name);
    app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), legacy->mqtt_backend);
    app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), legacy->mqtt_host);
    app_copy_string(s_config.mqtt_token, sizeof(s_config.mqtt_token), legacy->mqtt_token);
    app_sanitize_gpio_config();
}

static void app_migrate_config_v4(const app_config_v4_t *legacy)
{
    app_set_defaults();
    s_config.relay_active_high = legacy->relay_active_high;
    s_config.led_active_high = legacy->led_active_high;
    s_config.input_active_low = legacy->input_active_low;
    s_config.relay_on = legacy->relay_on;
    s_config.led_on = legacy->led_on;
    s_config.bt_mode = legacy->bt_mode;
    s_config.net_mode = legacy->net_mode;
    s_config.mqtt_use_tls = legacy->mqtt_use_tls;
    s_config.uart_parity_mode = legacy->uart_parity_mode;
    s_config.uart_data_bits = 8;
    s_config.uart_stop_bits = 1;
    s_config.mqtt_port = legacy->mqtt_port;
    s_config.uart_baudrate = legacy->uart_baudrate;
    app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), legacy->ap_ssid);
    app_copy_string(s_config.ap_password, sizeof(s_config.ap_password), legacy->ap_password);
    app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), legacy->sta_ssid);
    app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), legacy->sta_password);
    app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), legacy->bt_device_name);
    app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), legacy->mqtt_backend);
    app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), legacy->mqtt_host);
    app_copy_string(s_config.mqtt_token, sizeof(s_config.mqtt_token), legacy->mqtt_token);
    app_sanitize_gpio_config();
}

static void app_migrate_config_v5(const app_config_v5_t *legacy)
{
    app_set_defaults();
    s_config.relay_active_high = legacy->relay_active_high;
    s_config.led_active_high = legacy->led_active_high;
    s_config.input_active_low = legacy->input_active_low;
    s_config.relay_on = legacy->relay_on;
    s_config.led_on = legacy->led_on;
    s_config.bt_mode = legacy->bt_mode;
    s_config.net_mode = legacy->net_mode;
    s_config.mqtt_use_tls = legacy->mqtt_use_tls;
    s_config.uart_parity_mode = legacy->uart_parity_mode;
    s_config.uart_data_bits = 8;
    s_config.uart_stop_bits = 1;
    s_config.mqtt_port = legacy->mqtt_port;
    s_config.uart_baudrate = legacy->uart_baudrate;
    app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), legacy->ap_ssid);
    app_copy_string(s_config.ap_password, sizeof(s_config.ap_password), legacy->ap_password);
    app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), legacy->sta_ssid);
    app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), legacy->sta_password);
    app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), legacy->bt_device_name);
    app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), legacy->mqtt_backend);
    app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), legacy->mqtt_host);
    app_copy_string(s_config.mqtt_token, sizeof(s_config.mqtt_token), legacy->mqtt_token);
    app_sanitize_gpio_config();
}

static void app_load_config(void)
{
    nvs_handle_t nvs_handle;
    size_t size = 0;

    app_set_defaults();

    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS failed, using defaults");
        return;
    }

    if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, NULL, &size) != ESP_OK) {
        ESP_LOGI(TAG, "No compatible saved config found, storing defaults");
        app_set_defaults();
        nvs_set_blob(nvs_handle, APP_CONFIG_KEY, &s_config, sizeof(s_config));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        return;
    }

    if (size == sizeof(s_config)) {
        size_t current_size = sizeof(s_config);
        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &s_config, &current_size) == ESP_OK &&
            s_config.magic == APP_CONFIG_MAGIC &&
            s_config.version == APP_CONFIG_VERSION) {
            app_sanitize_gpio_config();
            nvs_close(nvs_handle);
            return;
        }
    } else if (size == sizeof(app_config_v5_t)) {
        app_config_v5_t legacy = {0};
        size_t legacy_size = sizeof(legacy);

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &legacy, &legacy_size) == ESP_OK &&
            legacy.magic == APP_CONFIG_MAGIC &&
            legacy.version == 5) {
            ESP_LOGI(TAG, "Migrating saved config from v5 to v6");
            app_migrate_config_v5(&legacy);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, &s_config, sizeof(s_config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    } else if (size == sizeof(app_config_v4_t)) {
        app_config_v4_t legacy = {0};
        size_t legacy_size = sizeof(legacy);

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &legacy, &legacy_size) == ESP_OK &&
            legacy.magic == APP_CONFIG_MAGIC &&
            legacy.version == 4) {
            ESP_LOGI(TAG, "Migrating saved config from v4 to v6");
            app_migrate_config_v4(&legacy);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, &s_config, sizeof(s_config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    } else if (size == sizeof(app_config_v3_t)) {
        app_config_v3_t legacy = {0};
        size_t legacy_size = sizeof(legacy);

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &legacy, &legacy_size) == ESP_OK &&
            legacy.magic == APP_CONFIG_MAGIC &&
            legacy.version == 3) {
            ESP_LOGI(TAG, "Migrating saved config from v3 to v6");
            app_migrate_config_v3(&legacy);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, &s_config, sizeof(s_config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    }

    ESP_LOGI(TAG, "No compatible saved config found, storing defaults");
    app_set_defaults();
    nvs_set_blob(nvs_handle, APP_CONFIG_KEY, &s_config, sizeof(s_config));
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

static void app_apply_output(gpio_num_t gpio_num, bool active_high, bool on)
{
    gpio_set_level(gpio_num, (active_high ? on : !on) ? 1 : 0);
}

static bool app_get_input_state(void)
{
    int raw = gpio_get_level(app_input_gpio());
    return s_config.input_active_low ? (raw == 0) : (raw != 0);
}

static void app_configure_gpio(void)
{
    const gpio_config_t output_cfg = {
        .pin_bit_mask = (1ULL << app_relay_gpio()) | (1ULL << app_led_gpio()),
        .mode = GPIO_MODE_OUTPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };
    const gpio_config_t input_cfg = {
        .pin_bit_mask = (1ULL << app_input_gpio()),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = app_input_gpio() >= GPIO_NUM_34 ? GPIO_PULLUP_DISABLE : GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_cfg));
    ESP_ERROR_CHECK(gpio_config(&input_cfg));

    app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
    app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
}

static esp_err_t app_mount_fs(void)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = APP_BASE_PATH,
        .partition_label = "storage",
        .format_if_mount_failed = true,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

static const char *app_wifi_authmode_to_string(wifi_auth_mode_t mode)
{
    switch (mode) {
    case WIFI_AUTH_OPEN:
        return "open";
    case WIFI_AUTH_WEP:
        return "wep";
    case WIFI_AUTH_WPA_PSK:
        return "wpa";
    case WIFI_AUTH_WPA2_PSK:
        return "wpa2";
    case WIFI_AUTH_WPA_WPA2_PSK:
        return "wpa_wpa2";
    case WIFI_AUTH_WPA2_ENTERPRISE:
        return "wpa2_enterprise";
    case WIFI_AUTH_WPA3_PSK:
        return "wpa3";
    case WIFI_AUTH_WPA2_WPA3_PSK:
        return "wpa2_wpa3";
    default:
        return "unknown";
    }
}

static void app_set_sta_ip_strings(const esp_netif_ip_info_t *ip_info)
{
    if (ip_info == NULL) {
        app_copy_string(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), "0.0.0.0");
        app_copy_string(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), "0.0.0.0");
        app_copy_string(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), "0.0.0.0");
        return;
    }

    snprintf(s_wifi.sta_ip, sizeof(s_wifi.sta_ip), IPSTR, IP2STR(&ip_info->ip));
    snprintf(s_wifi.sta_gateway, sizeof(s_wifi.sta_gateway), IPSTR, IP2STR(&ip_info->gw));
    snprintf(s_wifi.sta_netmask, sizeof(s_wifi.sta_netmask), IPSTR, IP2STR(&ip_info->netmask));
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

static bool app_network_mode_has_sta(uint8_t mode)
{
    return mode == NET_MODE_STA || mode == NET_MODE_APSTA;
}

static bool app_network_mode_has_ap(uint8_t mode)
{
    return mode == NET_MODE_AP || mode == NET_MODE_APSTA;
}

static esp_err_t app_configure_ap_netif_ip(void)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_err_t err = esp_netif_dhcps_stop(s_ap_netif);
    if (err != ESP_OK && err != ESP_ERR_ESP_NETIF_DHCP_ALREADY_STOPPED) {
        return err;
    }

    IP4_ADDR(&ip_info.ip, 192, 168, 8, 1);
    IP4_ADDR(&ip_info.gw, 192, 168, 8, 1);
    IP4_ADDR(&ip_info.netmask, 255, 255, 255, 0);
    ESP_ERROR_CHECK(esp_netif_set_ip_info(s_ap_netif, &ip_info));
    return esp_netif_dhcps_start(s_ap_netif);
}

static void app_wifi_event_handler(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    (void)arg;

    if (event_base == WIFI_EVENT) {
        switch (event_id) {
        case WIFI_EVENT_STA_START:
            if (app_network_mode_has_sta(s_config.net_mode) && strlen(s_config.sta_ssid) > 0) {
                esp_wifi_connect();
            }
            break;
        case WIFI_EVENT_STA_CONNECTED:
            s_wifi.sta_connected = true;
            s_wifi.sta_retries = 0;
            app_copy_string(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "none");
            break;
        case WIFI_EVENT_STA_DISCONNECTED: {
            wifi_event_sta_disconnected_t *disconnected = event_data;
            s_wifi.sta_connected = false;
            s_wifi.sta_has_ip = false;
            app_set_sta_ip_strings(NULL);
            snprintf(s_wifi.last_disconnect, sizeof(s_wifi.last_disconnect), "reason_%d", disconnected->reason);
            app_mqtt_stop();
            if (app_network_mode_has_sta(s_config.net_mode) &&
                strlen(s_config.sta_ssid) > 0 &&
                s_wifi.sta_retries < 5) {
                s_wifi.sta_retries++;
                esp_wifi_connect();
            }
            break;
        }
        default:
            break;
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = event_data;
        s_wifi.sta_connected = true;
        s_wifi.sta_has_ip = true;
        s_wifi.sta_retries = 0;
        app_set_sta_ip_strings(&event->ip_info);
        xTaskCreate(app_mqtt_restart_task, "mqtt_restart_ip", 4096, NULL, 5, NULL);
    }
}

static esp_err_t app_apply_wifi_config(void)
{
    wifi_config_t ap_cfg = {0};
    wifi_config_t sta_cfg = {0};
    esp_err_t err;

    app_copy_string((char *)ap_cfg.ap.ssid, sizeof(ap_cfg.ap.ssid), s_config.ap_ssid);
    ap_cfg.ap.ssid_len = strlen((char *)ap_cfg.ap.ssid);
    app_copy_string((char *)ap_cfg.ap.password, sizeof(ap_cfg.ap.password), s_config.ap_password);
    ap_cfg.ap.channel = 1;
    ap_cfg.ap.max_connection = 4;
    ap_cfg.ap.authmode = strlen(s_config.ap_password) == 0 ? WIFI_AUTH_OPEN : WIFI_AUTH_WPA2_PSK;
    ap_cfg.ap.pmf_cfg.required = false;

    app_copy_string((char *)sta_cfg.sta.ssid, sizeof(sta_cfg.sta.ssid), s_config.sta_ssid);
    app_copy_string((char *)sta_cfg.sta.password, sizeof(sta_cfg.sta.password), s_config.sta_password);
    sta_cfg.sta.threshold.authmode = WIFI_AUTH_OPEN;
    sta_cfg.sta.pmf_cfg.capable = true;
    sta_cfg.sta.pmf_cfg.required = false;
    sta_cfg.sta.scan_method = WIFI_ALL_CHANNEL_SCAN;

    app_mqtt_stop();
    app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "waiting_sta_ip");

    err = esp_wifi_stop();
    if (err != ESP_OK && err != ESP_ERR_WIFI_NOT_STARTED && err != ESP_ERR_WIFI_NOT_INIT) {
        return err;
    }

    s_wifi.sta_connected = false;
    s_wifi.sta_has_ip = false;
    s_wifi.sta_retries = 0;
    app_set_sta_ip_strings(NULL);

    ESP_ERROR_CHECK(esp_wifi_set_mode(app_network_mode_to_wifi_mode(s_config.net_mode)));
    if (app_network_mode_has_ap(s_config.net_mode)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_cfg));
        ESP_ERROR_CHECK(app_configure_ap_netif_ip());
    }
    if (app_network_mode_has_sta(s_config.net_mode)) {
        ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_cfg));
    }

    ESP_ERROR_CHECK(esp_wifi_start());
    if (app_network_mode_has_sta(s_config.net_mode) && strlen(s_config.sta_ssid) > 0) {
        ESP_ERROR_CHECK(esp_wifi_connect());
    }
    return ESP_OK;
}

static esp_err_t app_wifi_perform_scan(void)
{
    wifi_mode_t old_mode = WIFI_MODE_NULL;
    bool temporary_apsta = false;
    uint16_t number = WIFI_SCAN_LIST_SIZE;

    ESP_ERROR_CHECK(esp_wifi_get_mode(&old_mode));
    if (old_mode == WIFI_MODE_AP) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
        temporary_apsta = true;
        vTaskDelay(pdMS_TO_TICKS(120));
    }

    ESP_ERROR_CHECK(esp_wifi_scan_start(NULL, true));
    ESP_ERROR_CHECK(esp_wifi_scan_get_ap_records(&number, s_wifi.scan_records));
    s_wifi.scan_count = number;

    if (temporary_apsta) {
        ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_AP));
    }
    return ESP_OK;
}

static void app_wifi_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(250));
    if (app_apply_wifi_config() != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi reconfigure failed");
    }
    vTaskDelete(NULL);
}

static void app_start_wifi(void)
{
    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());
    s_ap_netif = esp_netif_create_default_wifi_ap();
    s_sta_netif = esp_netif_create_default_wifi_sta();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));
    ESP_ERROR_CHECK(esp_wifi_set_storage(WIFI_STORAGE_RAM));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &app_wifi_event_handler, NULL));
    ESP_ERROR_CHECK(app_apply_wifi_config());
}

static esp_err_t http_send_json_text(httpd_req_t *req, const char *status, const char *payload)
{
    if (status != NULL) {
        httpd_resp_set_status(req, status);
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    return httpd_resp_sendstr(req, payload);
}

static esp_err_t http_send_jsonf(httpd_req_t *req, const char *status, const char *fmt, ...)
{
    char *payload = malloc(APP_JSON_BUFFER_SIZE);
    va_list args;
    esp_err_t err;

    if (payload == NULL) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    va_start(args, fmt);
    vsnprintf(payload, APP_JSON_BUFFER_SIZE, fmt, args);
    va_end(args);
    err = http_send_json_text(req, status, payload);
    free(payload);
    return err;
}

static esp_err_t http_read_body(httpd_req_t *req, char *buf, size_t buf_len)
{
    int remaining = req->content_len;
    int received = 0;

    if (remaining >= (int)buf_len) {
        return http_send_json_text(req, "413 Payload Too Large",
                                   "{\"status\":\"error\",\"message\":\"payload_too_large\"}");
    }

    while (remaining > 0) {
        int ret = httpd_req_recv(req, buf + received, remaining);
        if (ret <= 0) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"body_read_failed\"}");
        }
        received += ret;
        remaining -= ret;
    }

    buf[received] = '\0';
    return ESP_OK;
}

static bool json_find_string(const char *json, const char *key, char *out, size_t out_size)
{
    char pattern[48];
    const char *pos;
    size_t idx = 0;

    snprintf(pattern, sizeof(pattern), "\"%s\":\"", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        snprintf(pattern, sizeof(pattern), "\"%s\": \"", key);
        pos = strstr(json, pattern);
        if (pos == NULL) {
            return false;
        }
    }

    pos += strlen(pattern);
    while (*pos != '\0' && idx + 1 < out_size) {
        if (*pos == '"') {
            break;
        }
        if (*pos == '\\' && pos[1] != '\0') {
            pos++;
            switch (*pos) {
            case 'n':
                out[idx++] = '\n';
                break;
            case 'r':
                out[idx++] = '\r';
                break;
            case 't':
                out[idx++] = '\t';
                break;
            case '"':
                out[idx++] = '"';
                break;
            case '\\':
                out[idx++] = '\\';
                break;
            default:
                out[idx++] = *pos;
                break;
            }
            pos++;
            continue;
        }
        out[idx++] = *pos++;
    }
    out[idx] = '\0';
    return true;
}

static bool json_find_bool(const char *json, const char *key, bool *value)
{
    char pattern[48];
    const char *pos;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        return false;
    }

    pos += strlen(pattern);
    while (*pos == ' ') {
        pos++;
    }
    if (strncmp(pos, "true", 4) == 0) {
        *value = true;
        return true;
    }
    if (strncmp(pos, "false", 5) == 0) {
        *value = false;
        return true;
    }
    return false;
}

static bool json_find_u16(const char *json, const char *key, uint16_t *value)
{
    char pattern[48];
    const char *pos;
    long parsed;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        return false;
    }

    pos += strlen(pattern);
    while (*pos == ' ') {
        pos++;
    }

    parsed = strtol(pos, NULL, 10);
    if (parsed < 0 || parsed > 65535) {
        return false;
    }

    *value = (uint16_t)parsed;
    return true;
}

static bool json_find_u32(const char *json, const char *key, uint32_t *value)
{
    char pattern[48];
    const char *pos;
    unsigned long parsed;

    snprintf(pattern, sizeof(pattern), "\"%s\":", key);
    pos = strstr(json, pattern);
    if (pos == NULL) {
        return false;
    }

    pos += strlen(pattern);
    while (*pos == ' ') {
        pos++;
    }

    parsed = strtoul(pos, NULL, 10);
    *value = (uint32_t)parsed;
    return true;
}

static esp_err_t http_serve_index(httpd_req_t *req)
{
    char path[96];
    FILE *file;
    char chunk[256];
    size_t read_bytes = 0;

    snprintf(path, sizeof(path), "%s/index.html", APP_BASE_PATH);
    file = fopen(path, "rb");
    if (file == NULL) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"index_open_failed\"}");
    }

    httpd_resp_set_type(req, "text/html; charset=utf-8");
    do {
        read_bytes = fread(chunk, 1, sizeof(chunk), file);
        if (read_bytes > 0) {
            if (httpd_resp_send_chunk(req, chunk, read_bytes) != ESP_OK) {
                fclose(file);
                return ESP_FAIL;
            }
        }
    } while (read_bytes > 0);

    fclose(file);
    return httpd_resp_send_chunk(req, NULL, 0);
}

static void app_get_ap_network_strings(char *ip, size_t ip_len, char *gateway, size_t gw_len,
                                       char *netmask, size_t netmask_len)
{
    esp_netif_ip_info_t ip_info = {0};
    esp_netif_get_ip_info(s_ap_netif, &ip_info);
    snprintf(ip, ip_len, IPSTR, IP2STR(&ip_info.ip));
    snprintf(gateway, gw_len, IPSTR, IP2STR(&ip_info.gw));
    snprintf(netmask, netmask_len, IPSTR, IP2STR(&ip_info.netmask));
}

static void app_get_storage_stats(size_t *total_bytes, size_t *used_bytes)
{
    size_t total = 0;
    size_t used = 0;

    if (esp_littlefs_info("storage", &total, &used) != ESP_OK) {
        total = 0;
        used = 0;
    }

    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        *used_bytes = used;
    }
}

static void app_get_running_partition_stats(const esp_partition_t **running_partition,
                                            size_t *total_bytes,
                                            size_t *used_bytes)
{
    const esp_partition_t *partition = esp_ota_get_running_partition();
    size_t total = 0;
    size_t used = 0;

    if (partition != NULL) {
        esp_image_metadata_t metadata = {0};
        const esp_partition_pos_t part_pos = {
            .offset = partition->address,
            .size = partition->size,
        };
        total = partition->size;
        if (esp_image_get_metadata(&part_pos, &metadata) == ESP_OK) {
            used = metadata.image_len;
        }
    }

    if (running_partition != NULL) {
        *running_partition = partition;
    }
    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        *used_bytes = used;
    }
}

static void app_bt_gap_cb(esp_bt_gap_cb_event_t event, esp_bt_gap_cb_param_t *param)
{
    if (event == ESP_BT_GAP_MODE_CHG_EVT) {
        ESP_LOGI(TAG, "Classic BT mode changed: %d", param->mode_chg.mode);
    }
}

static void app_ble_gap_cb(esp_gap_ble_cb_event_t event, esp_ble_gap_cb_param_t *param)
{
    switch (event) {
    case ESP_GAP_BLE_ADV_DATA_SET_COMPLETE_EVT:
        s_ble_adv_configured = true;
        esp_ble_gap_start_advertising(&s_ble_adv_params);
        break;
    case ESP_GAP_BLE_ADV_START_COMPLETE_EVT:
        if (param->adv_start_cmpl.status == ESP_BT_STATUS_SUCCESS) {
            ESP_LOGI(TAG, "BLE advertising started");
            app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "");
        } else {
            ESP_LOGE(TAG, "BLE advertising start failed: %d", param->adv_start_cmpl.status);
            snprintf(s_bt_last_error, sizeof(s_bt_last_error), "adv_start:%d", param->adv_start_cmpl.status);
            s_bt_runtime_mode = BT_MODE_OFF;
        }
        break;
    default:
        break;
    }
}

static void app_spp_cb(esp_spp_cb_event_t event, esp_spp_cb_param_t *param)
{
    switch (event) {
    case ESP_SPP_INIT_EVT:
        if (param->init.status == ESP_SPP_SUCCESS) {
            esp_spp_start_srv(ESP_SPP_SEC_NONE, ESP_SPP_ROLE_SLAVE, 0, SPP_SERVER_NAME);
        }
        break;
    case ESP_SPP_START_EVT:
        if (param->start.status == ESP_SPP_SUCCESS) {
            s_spp_ready = true;
            esp_bt_gap_set_device_name(s_config.bt_device_name);
            esp_bt_gap_set_scan_mode(ESP_BT_CONNECTABLE, ESP_BT_GENERAL_DISCOVERABLE);
            ESP_LOGI(TAG, "SPP server started");
        }
        break;
    case ESP_SPP_SRV_OPEN_EVT:
        memcpy(s_spp_remote_bda, param->srv_open.rem_bda, sizeof(s_spp_remote_bda));
        ESP_LOGI(TAG, "SPP client connected");
        break;
    case ESP_SPP_CLOSE_EVT:
        memset(s_spp_remote_bda, 0, sizeof(s_spp_remote_bda));
        ESP_LOGI(TAG, "SPP client disconnected");
        break;
    default:
        break;
    }
}

static void app_bt_stop(void)
{
    esp_bluedroid_status_t bluedroid_status = esp_bluedroid_get_status();
    esp_bt_controller_status_t controller_status = esp_bt_controller_get_status();

    if (s_bt_runtime_mode == BT_MODE_SPP && s_spp_ready) {
        esp_spp_deinit();
        s_spp_ready = false;
    }
    if (s_bt_runtime_mode == BT_MODE_BLE && s_ble_adv_configured) {
        esp_ble_gap_stop_advertising();
        s_ble_adv_configured = false;
    }

    if (bluedroid_status == ESP_BLUEDROID_STATUS_ENABLED) {
        esp_bluedroid_disable();
    }
    if (bluedroid_status != ESP_BLUEDROID_STATUS_UNINITIALIZED) {
        esp_bluedroid_deinit();
    }
    if (controller_status == ESP_BT_CONTROLLER_STATUS_ENABLED) {
        esp_bt_controller_disable();
    }
    if (controller_status != ESP_BT_CONTROLLER_STATUS_IDLE) {
        esp_bt_controller_deinit();
    }

    s_bt_runtime_mode = BT_MODE_OFF;
    app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "");
}

static esp_err_t app_bt_start_ble(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_err_t err;

    err = esp_bt_controller_init(&bt_cfg);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "controller_init:%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bt_controller_enable(ESP_BT_MODE_BTDM);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "controller_enable:%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_init_with_cfg(&bluedroid_cfg);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "bluedroid_init:%s", esp_err_to_name(err));
        return err;
    }
    err = esp_bluedroid_enable();
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "bluedroid_enable:%s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gap_register_callback(app_ble_gap_cb);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "gap_callback:%s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gap_set_device_name(s_config.bt_device_name);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "set_name:%s", esp_err_to_name(err));
        return err;
    }
    err = esp_ble_gap_config_adv_data(&s_ble_adv_data);
    if (err != ESP_OK) {
        snprintf(s_bt_last_error, sizeof(s_bt_last_error), "adv_config:%s", esp_err_to_name(err));
        return err;
    }
    s_bt_runtime_mode = BT_MODE_BLE;
    app_copy_string(s_bt_last_error, sizeof(s_bt_last_error), "ble_starting");
    return ESP_OK;
}

static esp_err_t app_bt_start_spp(void)
{
    esp_bt_controller_config_t bt_cfg = BT_CONTROLLER_INIT_CONFIG_DEFAULT();
    esp_bluedroid_config_t bluedroid_cfg = BT_BLUEDROID_INIT_CONFIG_DEFAULT();
    esp_spp_cfg_t spp_cfg = {
        .mode = ESP_SPP_MODE_CB,
        .enable_l2cap_ertm = true,
        .tx_buffer_size = 0,
    };
    esp_bt_pin_type_t pin_type = ESP_BT_PIN_TYPE_VARIABLE;
    esp_bt_pin_code_t pin_code = {0};

    ESP_RETURN_ON_ERROR(esp_bt_controller_init(&bt_cfg), TAG, "controller init failed");
    ESP_RETURN_ON_ERROR(esp_bt_controller_enable(ESP_BT_MODE_BTDM), TAG, "controller enable failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_init_with_cfg(&bluedroid_cfg), TAG, "bluedroid init failed");
    ESP_RETURN_ON_ERROR(esp_bluedroid_enable(), TAG, "bluedroid enable failed");
    ESP_RETURN_ON_ERROR(esp_bt_gap_register_callback(app_bt_gap_cb), TAG, "bt gap register failed");
    ESP_RETURN_ON_ERROR(esp_spp_register_callback(app_spp_cb), TAG, "spp callback register failed");
    ESP_RETURN_ON_ERROR(esp_spp_enhanced_init(&spp_cfg), TAG, "spp init failed");
    esp_bt_gap_set_pin(pin_type, 0, pin_code);
    s_bt_runtime_mode = BT_MODE_SPP;
    return ESP_OK;
}

static esp_err_t app_bt_apply_config(void)
{
    app_bt_stop();

    if (s_config.bt_mode == BT_MODE_OFF) {
        return ESP_OK;
    }
    if (s_config.bt_mode == BT_MODE_BLE) {
        return app_bt_start_ble();
    }
    if (s_config.bt_mode == BT_MODE_SPP) {
        return app_bt_start_spp();
    }
    return ESP_ERR_INVALID_ARG;
}

static void app_bt_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    if (app_bt_apply_config() != ESP_OK) {
        ESP_LOGE(TAG, "Bluetooth apply config failed");
    }
    vTaskDelete(NULL);
}

static uint32_t app_uart_normalize_baudrate(uint32_t measured)
{
    // 把 RMT 解算的 baud（或用户输入的 baud）归一化到最接近的标准波特率。
    // 注意：RMT 方案不会再出现 16× 误判，无需反向校正。
    static const uint32_t candidates[] = {
        50, 75, 110, 134, 150, 200, 300, 600,
        1200, 1800, 2000, 2400, 3600, 4800, 7200, 9600,
        10400, 14400, 19200, 28800, 31250, 38400, 56000, 57600,
        74880, 76800, 115200, 128000, 153600, 230400, 250000, 256000,
        460800, 500000, 512000, 576000, 921600, 1000000, 1152000, 1500000,
        2000000, 2500000, 3000000, 3500000, 4000000, 5000000
    };
    uint32_t best = candidates[0];
    uint32_t best_diff = measured > best ? measured - best : best - measured;
    size_t index;

    for (index = 1; index < sizeof(candidates) / sizeof(candidates[0]); index++) {
        uint32_t diff = measured > candidates[index] ? measured - candidates[index] : candidates[index] - measured;
        if (diff < best_diff) {
            best = candidates[index];
            best_diff = diff;
        }
    }

    return best;
}

static void app_uart_add_baud_candidate(uint32_t *list, size_t list_size, size_t *count, uint32_t baudrate)
{
    size_t index;

    if (baudrate == 0 || *count >= list_size) {
        return;
    }

    for (index = 0; index < *count; index++) {
        if (list[index] == baudrate) {
            return;
        }
    }

    list[*count] = baudrate;
    (*count)++;
}

static size_t app_uart_build_priority_baud_list(uint32_t preferred_baudrate, uint32_t raw_baudrate,
                                                bool probe_reliable, uint32_t *out_list, size_t out_size)
{
    static const uint32_t common_candidates[] = {
        9600, 4800, 2400, 1200, 3600, 19200, 38400, 57600,
        115200, 14400, 28800, 74880, 230400, 460800
    };
    static const uint32_t divisors[] = {16, 8, 4, 2};
    size_t count = 0;
    size_t index;

    // 注意：手动探测会话的优先起点由调用方通过 preferred_baudrate 传入。
    // 绝对不要把 s_config.uart_baudrate 加进来，否则 saved config 会污染探测起点。
    app_uart_add_baud_candidate(out_list, out_size, &count, preferred_baudrate);

    if (raw_baudrate > 0) {
        for (index = 0; index < sizeof(divisors) / sizeof(divisors[0]); index++) {
            if (raw_baudrate > divisors[index]) {
                app_uart_add_baud_candidate(out_list, out_size, &count,
                                            app_uart_normalize_baudrate(raw_baudrate / divisors[index]));
            }
        }
    }

    for (index = 0; index < sizeof(common_candidates) / sizeof(common_candidates[0]); index++) {
        app_uart_add_baud_candidate(out_list, out_size, &count, common_candidates[index]);
    }

    if (!probe_reliable && raw_baudrate >= 16) {
        app_uart_add_baud_candidate(out_list, out_size, &count,
                                    app_uart_normalize_baudrate(raw_baudrate / 16));
    }

    if (raw_baudrate > 0) {
        app_uart_add_baud_candidate(out_list, out_size, &count,
                                    app_uart_normalize_baudrate(raw_baudrate));
    }

    return count;
}

static void app_uart_add_probe_candidate(uart_probe_candidate_t *list,
                                         size_t list_size,
                                         size_t *count,
                                         uint32_t baudrate,
                                         uart_parity_t parity)
{
    size_t index;

    if (baudrate == 0 || *count >= list_size) {
        return;
    }

    for (index = 0; index < *count; index++) {
        if (list[index].baudrate == baudrate && list[index].parity == parity) {
            return;
        }
    }

    list[*count].baudrate = baudrate;
    list[*count].parity = parity;
    (*count)++;
}

static size_t app_uart_build_manual_candidate_list(uint32_t preferred_baudrate,
                                                   uint32_t raw_baudrate,
                                                   bool probe_reliable,
                                                   uint32_t exclude_baudrate,
                                                   uart_parity_t exclude_parity,
                                                   uart_probe_candidate_t *out_list,
                                                   size_t out_size)
{
    static const uart_parity_t parity_order[] = {
        UART_PARITY_DISABLE,
        UART_PARITY_EVEN,
        UART_PARITY_ODD,
    };
    uint32_t baud_candidates[UART_MANUAL_CANDIDATE_MAX] = {0};
    size_t baud_count;
    size_t parity_index;
    size_t baud_index;
    size_t count = 0;

    baud_count = app_uart_build_priority_baud_list(preferred_baudrate, raw_baudrate,
                                                   probe_reliable, baud_candidates,
                                                   sizeof(baud_candidates) / sizeof(baud_candidates[0]));
    for (parity_index = 0; parity_index < sizeof(parity_order) / sizeof(parity_order[0]); parity_index++) {
        for (baud_index = 0; baud_index < baud_count; baud_index++) {
            if (baud_candidates[baud_index] == exclude_baudrate &&
                parity_order[parity_index] == exclude_parity) {
                continue;
            }
            app_uart_add_probe_candidate(out_list, out_size, &count,
                                         baud_candidates[baud_index], parity_order[parity_index]);
        }
    }

    return count;
}

static int app_uart_probe_score_current(void)
{
    int score;

    if (s_uart.last_bytes == 0) {
        return -1;
    }

    score = (int)s_uart.last_bytes * 8 + s_uart.ascii_ratio_pct * 4;

    if (strcmp(s_uart.protocol_hint, "scale_ascii") == 0) {
        score += 200;
    } else if (strcmp(s_uart.protocol_hint, "ascii") == 0) {
        score += 120;
    } else if (strcmp(s_uart.protocol_hint, "modbus_or_binary") == 0) {
        score += 80;
    }

    if (s_uart.last_bytes >= 8) {
        score += 40;
    }

    if (s_uart.ascii_ratio_pct >= 90 && s_uart.last_bytes < 12) {
        score -= (int)(12 - s_uart.last_bytes) * 22;
    }

    if (s_uart.last_bytes <= 4) {
        score -= 60;
    }

    return score;
}

static void app_uart_reset_manual_session(void)
{
    size_t index;

    s_uart.manual_session_active = false;
    s_uart.manual_candidate_count = 0;
    s_uart.manual_candidate_index = 0;

    for (index = 0; index < UART_MANUAL_CANDIDATE_MAX; index++) {
        s_uart.manual_candidates[index].baudrate = 0;
        s_uart.manual_candidates[index].parity = UART_PARITY_DISABLE;
    }
}

static void app_uart_reset_probe_metrics(void)
{
    size_t index;

    s_uart.probe_reliable = false;
    s_uart.probe_pending_confirmation = false;
    s_uart.probe_confirmed = false;
    s_uart.raw_baudrate = 0;
    s_uart.edge_count = 0;
    s_uart.low_period = 0;
    s_uart.high_period = 0;
    s_uart.pos_period = 0;
    s_uart.neg_period = 0;
    s_uart.clk_freq_hz = 0;
    s_uart.probe_best_score = -1;
    s_uart.probe_confirm_score = -1;
    s_uart.probe_result_count = 0;
    s_uart.pending_baudrate = 0;
    s_uart.detected_data_bits = 8;
    s_uart.detected_stop_bits = 1;
    s_uart.pending_data_bits = 8;
    s_uart.pending_stop_bits = 1;
    s_uart.pending_parity = UART_PARITY_DISABLE;
    app_uart_set_apply_error("");
    app_copy_string(s_uart.probe_source, sizeof(s_uart.probe_source), "none");
    app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "idle");
    app_uart_reset_manual_session();

    for (index = 0; index < UART_PROBE_RESULT_MAX; index++) {
        s_uart.probe_results[index].baudrate = 0;
        s_uart.probe_results[index].parity = UART_PARITY_DISABLE;
        s_uart.probe_results[index].score = -1;
        s_uart.probe_results[index].sample_bytes = 0;
        s_uart.probe_results[index].ascii_ratio_pct = 0;
        app_copy_string(s_uart.probe_results[index].protocol_hint,
                        sizeof(s_uart.probe_results[index].protocol_hint), "unknown");
        app_copy_string(s_uart.probe_results[index].sample,
                        sizeof(s_uart.probe_results[index].sample), "");
    }
}

static void app_uart_store_probe_result(uint32_t baudrate, uart_parity_t parity, int score)
{
    uart_probe_result_t entry;
    size_t insert_at = 0;
    size_t move_count;

    if (score < 0) {
        return;
    }

    entry.baudrate = baudrate;
    entry.parity = parity;
    entry.score = score;
    entry.sample_bytes = s_uart.last_bytes;
    entry.ascii_ratio_pct = s_uart.ascii_ratio_pct;
    app_copy_string(entry.protocol_hint, sizeof(entry.protocol_hint), s_uart.protocol_hint);
    app_copy_string(entry.sample, sizeof(entry.sample), s_uart.last_ascii);

    while (insert_at < s_uart.probe_result_count &&
           s_uart.probe_results[insert_at].score >= score) {
        insert_at++;
    }

    if (insert_at >= UART_PROBE_RESULT_MAX) {
        return;
    }

    if (s_uart.probe_result_count < UART_PROBE_RESULT_MAX) {
        s_uart.probe_result_count++;
    }

    move_count = s_uart.probe_result_count - insert_at - 1;
    while (move_count > 0) {
        s_uart.probe_results[insert_at + move_count] = s_uart.probe_results[insert_at + move_count - 1];
        move_count--;
    }

    s_uart.probe_results[insert_at] = entry;
}

static void app_uart_build_probe_attempts_json(char *dst, size_t dst_size)
{
    size_t index;
    size_t used = 0;

    if (dst_size == 0) {
        return;
    }

    dst[0] = '\0';
    used += snprintf(dst + used, dst_size - used, "[");

    for (index = 0; index < s_uart.probe_result_count && used + 1 < dst_size; index++) {
        char sample_json[UART_SAMPLE_BUFFER_SIZE * 2 + 1];
        int written;

        app_json_escape_string(sample_json, sizeof(sample_json), s_uart.probe_results[index].sample);
        written = snprintf(dst + used, dst_size - used,
                           "%s{\"baudrate\":%u,\"parity\":\"%s\",\"frame\":\"%s\","
                           "\"score\":%d,\"sample_bytes\":%u,\"ascii_ratio_pct\":%d,"
                           "\"protocol_hint\":\"%s\",\"sample\":\"%s\"}",
                           index == 0 ? "" : ",",
                           (unsigned)s_uart.probe_results[index].baudrate,
                           app_uart_parity_to_string(s_uart.probe_results[index].parity),
                           app_uart_parity_to_frame(s_uart.probe_results[index].parity),
                           s_uart.probe_results[index].score,
                           (unsigned)s_uart.probe_results[index].sample_bytes,
                           s_uart.probe_results[index].ascii_ratio_pct,
                           s_uart.probe_results[index].protocol_hint,
                           sample_json);
        if (written < 0) {
            break;
        }
        if ((size_t)written >= dst_size - used) {
            used = dst_size - 1;
            break;
        }
        used += (size_t)written;
    }

    if (used + 2 <= dst_size) {
        snprintf(dst + used, dst_size - used, "]");
    } else {
        dst[dst_size - 1] = '\0';
    }
}

static esp_err_t app_uart_send_runtime_json(httpd_req_t *req, const char *message)
{
    char *sample_json = NULL;
    char *attempts_json = NULL;
    char frame[8];
    char detected_frame[8];
    char pending_frame[8];
    esp_err_t err;
    bool manual_has_more = s_uart.manual_session_active &&
                           s_uart.manual_candidate_index < s_uart.manual_candidate_count;

    sample_json = malloc(UART_SAMPLE_BUFFER_SIZE * 2 + 1);
    attempts_json = malloc(1800);
    if (sample_json == NULL || attempts_json == NULL) {
        free(sample_json);
        free(attempts_json);
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"no_memory\"}");
    }

    app_json_escape_string(sample_json, UART_SAMPLE_BUFFER_SIZE * 2 + 1, s_uart.last_ascii);
    app_uart_build_probe_attempts_json(attempts_json, 1800);
    app_uart_build_frame(frame, sizeof(frame),
                         s_config.uart_data_bits,
                         (uart_parity_t)s_config.uart_parity_mode,
                         s_config.uart_stop_bits);
    app_uart_build_frame(detected_frame, sizeof(detected_frame),
                         s_uart.detected_data_bits,
                         s_uart.detected_parity,
                         s_uart.detected_stop_bits);
    app_uart_build_frame(pending_frame, sizeof(pending_frame),
                         s_uart.pending_data_bits,
                         s_uart.pending_parity,
                         s_uart.pending_stop_bits);
    err = http_send_jsonf(req, NULL,
                          "{\"status\":\"ok\",\"message\":\"%s\","
                          "\"rx_gpio\":%ld,\"tx_gpio\":%ld,\"baudrate\":%u,"
                          "\"data_bits\":%u,\"stop_bits\":%u,\"parity\":\"%s\","
                          "\"frame\":\"%s\",\"detected_baudrate\":%u,\"detected_parity\":\"%s\","
                          "\"detected_data_bits\":%u,\"detected_stop_bits\":%u,"
                          "\"raw_baudrate\":%u,\"probe_reliable\":%s,\"probe_source\":\"%s\","
                          "\"edge_count\":%u,\"clk_freq_hz\":%u,\"low_period\":%u,\"high_period\":%u,"
                          "\"pos_period\":%u,\"neg_period\":%u,"
                          "\"sample_bytes\":%u,\"ascii_ratio_pct\":%d,\"protocol_hint\":\"%s\","
                          "\"sample\":\"%s\",\"probe_pending_confirmation\":%s,"
                          "\"probe_confirmed\":%s,\"probe_mode\":\"%s\",\"probe_best_score\":%d,"
                          "\"probe_confirm_score\":%d,\"probe_attempts\":%s,"
                          "\"pending_baudrate\":%u,\"pending_parity\":\"%s\","
                          "\"pending_data_bits\":%u,\"pending_stop_bits\":%u,\"pending_frame\":\"%s\","
                          "\"manual_session_active\":%s,\"manual_candidate_index\":%u,"
                          "\"manual_candidate_count\":%u,\"manual_has_more\":%s}",
                          message != NULL ? message : "",
                          (long)app_uart_rx_gpio(),
                          (long)app_uart_tx_gpio(),
                          s_config.uart_baudrate,
                          (unsigned)s_config.uart_data_bits,
                          (unsigned)s_config.uart_stop_bits,
                          app_uart_parity_to_string((uart_parity_t)s_config.uart_parity_mode),
                          frame,
                          s_uart.detected_baudrate,
                          app_uart_parity_to_string(s_uart.detected_parity),
                          (unsigned)s_uart.detected_data_bits,
                          (unsigned)s_uart.detected_stop_bits,
                          s_uart.raw_baudrate,
                          s_uart.probe_reliable ? "true" : "false",
                          s_uart.probe_source,
                          s_uart.edge_count,
                          s_uart.clk_freq_hz,
                          s_uart.low_period,
                          s_uart.high_period,
                          s_uart.pos_period,
                          s_uart.neg_period,
                          (unsigned)s_uart.last_bytes,
                          s_uart.ascii_ratio_pct,
                          s_uart.protocol_hint,
                          sample_json,
                          s_uart.probe_pending_confirmation ? "true" : "false",
                          s_uart.probe_confirmed ? "true" : "false",
                          s_uart.probe_mode,
                          s_uart.probe_best_score,
                          s_uart.probe_confirm_score,
                          attempts_json,
                          s_uart.pending_baudrate,
                          app_uart_parity_to_string(s_uart.pending_parity),
                          (unsigned)s_uart.pending_data_bits,
                          (unsigned)s_uart.pending_stop_bits,
                          pending_frame,
                          s_uart.manual_session_active ? "true" : "false",
                          (unsigned)s_uart.manual_candidate_index,
                          (unsigned)s_uart.manual_candidate_count,
                          manual_has_more ? "true" : "false");
    free(sample_json);
    free(attempts_json);
    return err;
}

static void app_uart_update_protocol_hint(void)
{
    if (s_uart.last_bytes == 0) {
        app_copy_string(s_uart.protocol_hint, sizeof(s_uart.protocol_hint), "unknown");
        return;
    }

    if (s_uart.ascii_ratio_pct >= 85) {
        if (strstr(s_uart.last_ascii, "kg") != NULL || strstr(s_uart.last_ascii, "g") != NULL ||
            strstr(s_uart.last_ascii, "lb") != NULL) {
            app_copy_string(s_uart.protocol_hint, sizeof(s_uart.protocol_hint), "scale_ascii");
        } else {
            app_copy_string(s_uart.protocol_hint, sizeof(s_uart.protocol_hint), "ascii");
        }
    } else if (s_uart.last_bytes >= 4) {
        app_copy_string(s_uart.protocol_hint, sizeof(s_uart.protocol_hint), "modbus_or_binary");
    } else {
        app_copy_string(s_uart.protocol_hint, sizeof(s_uart.protocol_hint), "unknown");
    }
}

static void app_uart_console_reset(void)
{
    s_uart.rx_sequence = 0;
    app_copy_string(s_uart.last_ascii, sizeof(s_uart.last_ascii), "");
    app_copy_string(s_uart.rx_plain, sizeof(s_uart.rx_plain), "");
    app_copy_string(s_uart.rx_hex, sizeof(s_uart.rx_hex), "");
    s_uart.last_bytes = 0;
    s_uart.ascii_ratio_pct = 0;
    app_uart_update_protocol_hint();
}

static void app_append_tail(char *dst, size_t dst_size, const char *suffix)
{
    size_t current_len;
    size_t suffix_len;
    size_t overflow;

    if (dst_size == 0) {
        return;
    }

    current_len = strlen(dst);
    suffix_len = strlen(suffix);

    if (suffix_len >= dst_size) {
        memcpy(dst, suffix + suffix_len - (dst_size - 1), dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }

    if (current_len + suffix_len < dst_size) {
        memcpy(dst + current_len, suffix, suffix_len + 1);
        return;
    }

    overflow = current_len + suffix_len - (dst_size - 1);
    memmove(dst, dst + overflow, current_len - overflow + 1);
    current_len = strlen(dst);
    memcpy(dst + current_len, suffix, suffix_len + 1);
}

static void app_uart_append_console(const uint8_t *raw, size_t raw_len)
{
    char plain_chunk[UART_SAMPLE_BUFFER_SIZE + 2];
    char hex_chunk[UART_SAMPLE_BUFFER_SIZE * 3 + 2];
    size_t plain_index = 0;
    size_t hex_index = 0;
    size_t index;

    for (index = 0; index < raw_len; index++) {
        uint8_t ch = raw[index];
        bool is_printable = (ch >= 32 && ch <= 126) || ch == '\r' || ch == '\n' || ch == '\t';

        if (plain_index + 1 < sizeof(plain_chunk)) {
            plain_chunk[plain_index++] = is_printable ? (char)ch : '.';
        }

        if (hex_index + 3 < sizeof(hex_chunk)) {
            hex_index += (size_t)snprintf(hex_chunk + hex_index,
                                          sizeof(hex_chunk) - hex_index,
                                          "%02X ",
                                          ch);
        }
    }

    if (plain_index > 0 && plain_chunk[plain_index - 1] != '\n' &&
        plain_index + 1 < sizeof(plain_chunk)) {
        plain_chunk[plain_index++] = '\n';
    }
    plain_chunk[plain_index] = '\0';

    if (hex_index > 0) {
        if (hex_chunk[hex_index - 1] == ' ') {
            hex_chunk[hex_index - 1] = '\n';
        } else if (hex_index + 1 < sizeof(hex_chunk)) {
            hex_chunk[hex_index++] = '\n';
            hex_chunk[hex_index] = '\0';
        }
    }
    hex_chunk[hex_index] = '\0';

    app_append_tail(s_uart.rx_plain, sizeof(s_uart.rx_plain), plain_chunk);
    app_append_tail(s_uart.rx_hex, sizeof(s_uart.rx_hex), hex_chunk);
    s_uart.rx_sequence++;
}

static void app_uart_update_from_raw(const uint8_t *raw, size_t raw_len)
{
    size_t index;
    int printable = 0;

    for (index = 0; index < raw_len && index < UART_SAMPLE_BUFFER_SIZE; index++) {
        uint8_t ch = raw[index];
        bool is_printable = (ch >= 32 && ch <= 126) || ch == '\r' || ch == '\n' || ch == '\t';

        if (is_printable) {
            printable++;
            s_uart.last_ascii[index] = (char)ch;
        } else {
            s_uart.last_ascii[index] = '.';
        }
    }
    s_uart.last_ascii[index] = '\0';
    s_uart.last_bytes = raw_len;
    s_uart.ascii_ratio_pct = raw_len == 0 ? 0 : (printable * 100) / (int)raw_len;
    app_uart_update_protocol_hint();
    app_uart_append_console(raw, raw_len);
}

static int app_uart_read_raw(uint8_t *raw, size_t raw_size, TickType_t wait_ticks)
{
    size_t available = 0;

    if (!s_uart.driver_installed || raw_size == 0) {
        return -1;
    }

    uart_get_buffered_data_len(UART_PORT, &available);
    if (available == 0 && wait_ticks > 0) {
        vTaskDelay(wait_ticks);
        uart_get_buffered_data_len(UART_PORT, &available);
    }

    if (available == 0) {
        return 0;
    }

    // 一次性读完 ring 里的可用数据（上限 = 调用方提供的 raw_size）。
    // 之前硬限到 96 字节导致 3600 等低波特率下 ringbuffer 持续填满而数据被 ESP-IDF 静默丢弃。
    if (available > raw_size) {
        available = raw_size;
    }

    return uart_read_bytes(UART_PORT, raw, available, pdMS_TO_TICKS(50));
}

static void app_uart_poll_console(TickType_t wait_ticks)
{
    uint8_t raw[UART_SAMPLE_BUFFER_SIZE];
    int read_len;

    if (s_uart_console_mutex == NULL) {
        return;
    }
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }
    read_len = app_uart_read_raw(raw, sizeof(raw), wait_ticks);

    if (read_len > 0) {
        app_uart_update_from_raw(raw, (size_t)read_len);
    }
    xSemaphoreGiveRecursive(s_uart_console_mutex);
}

static void app_uart_capture_sample(TickType_t wait_ticks)
{
    uint8_t raw[UART_SAMPLE_BUFFER_SIZE];
    int read_len;

    if (!s_uart.driver_installed) {
        return;
    }
    if (s_uart_console_mutex == NULL) {
        return;
    }
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        return;
    }

    read_len = app_uart_read_raw(raw, sizeof(raw), wait_ticks);
    if (read_len == 0) {
        s_uart.last_bytes = 0;
        s_uart.ascii_ratio_pct = 0;
        app_copy_string(s_uart.last_ascii, sizeof(s_uart.last_ascii), "");
        app_uart_update_protocol_hint();
        xSemaphoreGiveRecursive(s_uart_console_mutex);
        return;
    }
    if (read_len <= 0) {
        s_uart.last_bytes = 0;
        s_uart.ascii_ratio_pct = 0;
        app_copy_string(s_uart.last_ascii, sizeof(s_uart.last_ascii), "");
        app_uart_update_protocol_hint();
        xSemaphoreGiveRecursive(s_uart_console_mutex);
        return;
    }

    app_uart_update_from_raw(raw, (size_t)read_len);
    xSemaphoreGiveRecursive(s_uart_console_mutex);
}

static esp_err_t app_uart_install_driver(uint32_t baudrate, uint8_t data_bits,
                                         uart_parity_t parity, uint8_t stop_bits)
{
    esp_err_t err;
    uart_sclk_t source_clk;
    uart_config_t uart_config;
    int retry;

    // 3600 等低波特率在 80MHz APB 时钟下分频比精度差，ESP-IDF uart_param_config
    // 会返回 "baud rate unachievable" (ESP_FAIL)。切到 1MHz REF_TICK 时钟源
    // 可以解决。文档原话："The slower the frequency of the clock source,
    // the slower the bitrate can be measured"。
    source_clk = (baudrate < 4800) ? UART_SCLK_REF_TICK : UART_SCLK_DEFAULT;
    uart_config = (uart_config_t){
        .baud_rate = (int)baudrate,
        .data_bits = app_uart_data_bits_to_enum(data_bits),
        .parity = parity,
        .stop_bits = app_uart_stop_bits_to_enum(stop_bits),
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = source_clk,
    };

    // 探测路径在 detect_bitrate_stop 之后进入 install。ESP-IDF 6.0.2 在
    // uart_detect_bitrate_stop 退出后，硬件 UART_CLKDIV 等寄存器可能残留 bitrate
    // 模式期间的值；再加上 driver_installed 已被 detect 内 delete 清为 false，
    // install 入口的 delete 保护逻辑被跳过，导致 uart_param_config 写入 CLKDIV 失败。
    // 解决：无论 driver_installed 是否 true，入口都先 delete 一次 + 短暂延时。
    uart_driver_delete(UART_PORT);
    s_uart.driver_installed = false;
    vTaskDelay(pdMS_TO_TICKS(20));

    app_uart_set_apply_error("uart_apply_failed");

    // 探测路径（bitrate detect → install）有概率触发"硬件残留状态"。
    // 重试一次：先 delete → 短暂延时 → 再装。
    for (retry = 0; retry < 2; retry++) {
        if (s_uart.driver_installed) {
            uart_driver_delete(UART_PORT);
            s_uart.driver_installed = false;
        }
        if (retry > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        err = uart_param_config(UART_PORT, &uart_config);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_param_config_failed");
            ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
            continue;
        }

        err = uart_set_pin(UART_PORT, app_uart_tx_gpio(), app_uart_rx_gpio(),
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_set_pin_failed");
            ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
            continue;
        }

        err = uart_driver_install(UART_PORT, UART_RX_BUFFER_SIZE * 2, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_driver_install_failed");
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
            continue;
        }

        // 成功
        s_uart.driver_installed = true;
        app_uart_set_apply_error("");
        app_uart_console_reset();
        s_uart.detected_baudrate = baudrate;
        s_uart.detected_data_bits = data_bits;
        s_uart.detected_stop_bits = stop_bits;
        s_uart.detected_parity = parity;
        uart_flush_input(UART_PORT);
        return ESP_OK;
    }

    return err;
}

static esp_err_t app_uart_apply_config(void)
{
    return app_uart_install_driver(s_config.uart_baudrate,
                                   s_config.uart_data_bits,
                                   (uart_parity_t)s_config.uart_parity_mode,
                                   s_config.uart_stop_bits);
}

// 把"进入探测前那一刻的运行配置"保存到 restore_snapshot。
// 探测 handler 入口必须先调用，stop/失败 时才能用 restore_from_snapshot 还原。
// 注意：这里读的是 s_config（已保存的"真实配置"），不是页面当前填写值。
static void app_uart_capture_snapshot(void)
{
    s_uart.restore_snapshot.baudrate = s_config.uart_baudrate;
    s_uart.restore_snapshot.parity = (uart_parity_t)s_config.uart_parity_mode;
    s_uart.restore_snapshot.data_bits = s_config.uart_data_bits;
    s_uart.restore_snapshot.stop_bits = s_config.uart_stop_bits;
    s_uart.restore_snapshot_valid = true;
}

// 用 restore_snapshot 还原运行配置。
// 用途：探测失败 / 用户点 stop / 手动候选耗尽 等"退出探测"场景。
// 失败时设置 last_apply_error="uart_restore_failed"。
// 成功时不要清空 last_apply_error——之前的 RMT/手动轮询失败原因需要保留给前端读取。
// app_uart_install_driver 内部成功时会清空 error key，这里在调用前后保存/恢复。
// 不要在 confirm 成功后调用（confirm 已经把 pending 写回 s_config）。
static esp_err_t app_uart_restore_from_snapshot(void)
{
    if (!s_uart.restore_snapshot_valid) {
        app_uart_set_apply_error("uart_restore_failed");
        return ESP_ERR_INVALID_STATE;
    }
    // 保存"为什么要 restore"的失败原因，install_driver 内部会清空 error key。
    char saved_error[48];
    app_copy_string(saved_error, sizeof(saved_error), app_uart_get_apply_error());
    esp_err_t err = app_uart_install_driver(s_uart.restore_snapshot.baudrate,
                                            s_uart.restore_snapshot.data_bits,
                                            s_uart.restore_snapshot.parity,
                                            s_uart.restore_snapshot.stop_bits);
    if (err != ESP_OK) {
        app_uart_set_apply_error("uart_restore_failed");
        return err;
    }
    s_uart.restore_snapshot_valid = false;
    // 恢复之前的失败原因，让前端能读到。
    if (saved_error[0] != '\0') {
        app_uart_set_apply_error(saved_error);
    }
    return ESP_OK;
}

// RMT 探测接收完成回调。在 ISR 上下文中运行，置位信号量后立即返回。
// 数据拷贝到 s_rmt_probe_symbols 必须在 task 上下文做，所以这里只记状态。
static bool IRAM_ATTR app_rmt_probe_rx_done_cb(rmt_channel_handle_t channel,
                                               const rmt_rx_done_event_data_t *edata,
                                               void *user_data)
{
    BaseType_t higher_priority_task_woken = pdFALSE;
    if (s_rmt_probe_done_sem != NULL) {
        xSemaphoreGiveFromISR(s_rmt_probe_done_sem, &higher_priority_task_woken);
    }
    return higher_priority_task_woken == pdTRUE;
}

// 初始化 RMT 探测通道。分辨率 1 MHz (1 tick = 1 μs)。
// 1 MHz 下最低可测 baud = 1M/16 ≈ 62 bps，完全够 110/300/.../3600 等所有常见波特率。
static esp_err_t app_rmt_probe_init(void)
{
    if (s_rmt_probe_channel != NULL) {
        return ESP_OK;  // 已初始化
    }
    if (s_rmt_probe_done_sem == NULL) {
        s_rmt_probe_done_sem = xSemaphoreCreateBinary();
        if (s_rmt_probe_done_sem == NULL) {
            return ESP_ERR_NO_MEM;
        }
    }
    rmt_rx_channel_config_t rx_cfg = {
        .gpio_num = app_uart_rx_gpio(),
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 1000000,            // 1 MHz
        .mem_block_symbols = 64,             // 64 symbols/block；非 DMA 模式只能用单 block
    };
    esp_err_t err = rmt_new_rx_channel(&rx_cfg, &s_rmt_probe_channel);
    if (err != ESP_OK) {
        return err;
    }
    rmt_rx_event_callbacks_t cbs = {
        .on_recv_done = app_rmt_probe_rx_done_cb,
    };
    err = rmt_rx_register_event_callbacks(s_rmt_probe_channel, &cbs, NULL);
    if (err != ESP_OK) {
        rmt_del_channel(s_rmt_probe_channel);
        s_rmt_probe_channel = NULL;
        return err;
    }
    err = rmt_enable(s_rmt_probe_channel);
    if (err != ESP_OK) {
        rmt_del_channel(s_rmt_probe_channel);
        s_rmt_probe_channel = NULL;
        return err;
    }
    return ESP_OK;
}

// 释放 RMT 探测通道。每次探测结束后调用，确保资源不长期占用。
static void app_rmt_probe_deinit(void)
{
    if (s_rmt_probe_channel == NULL) {
        return;
    }
    rmt_disable(s_rmt_probe_channel);
    rmt_del_channel(s_rmt_probe_channel);
    s_rmt_probe_channel = NULL;
    s_rmt_probe_symbol_count = 0;
}

// 1 个 RMT symbol 包含两个电平段（level0/level1 + duration0/duration1）。
// 展开到 [duration, duration] 两个 pulse 数组，便于后续中位数统计。
typedef struct {
    uint16_t duration;
    uint8_t level;
} app_rmt_pulse_t;

// 从 s_rmt_probe_symbols 计算出 1 bit 时长（单位 ticks），进而得到 baud。
// 算法：
//   1. 收集所有 pulse（忽略 < 50us 毛刺和 > 1ms 长 pulse 认为是空闲）
//   2. 用直方图投票找出"出现次数最多的短 pulse"——这个就是 1 bit 时长
//   3. baud = 1,000,000 / one_bit_ticks
// 这样能抗毛刺、抗多 bit 累积（多个 bit 一起的 pulse 票数 < 1 bit 的票数）。
static esp_err_t app_rmt_compute_baud(uint32_t *out_baud)
{
    if (s_rmt_probe_symbol_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    // 直方图：bucket 大小 = 5 ticks = 5us，bucket 范围 50 ~ 2000 ticks（50us ~ 2ms）
    // 3600 baud 时 1 bit = 278us = 278 ticks → bucket 55
    // 9600 baud 时 1 bit = 104us = 104 ticks → bucket 20
    // 115200 baud 时 1 bit = 8.68us = 8.68 ticks → bucket 1.7 ≈ 2 (会被 < 50us 过滤掉)
    // 所以此算法主要针对 < 19200 baud 的常见工业设备波特率
    #define RMT_PULSE_MIN_TICKS   50
    #define RMT_PULSE_MAX_TICKS   2000
    #define RMT_BUCKET_TICKS      5
    #define RMT_BUCKET_COUNT      ((RMT_PULSE_MAX_TICKS - RMT_PULSE_MIN_TICKS) / RMT_BUCKET_TICKS + 1)
    static uint32_t histogram[RMT_BUCKET_COUNT];
    memset(histogram, 0, sizeof(histogram));
    size_t total_pulses = 0;
    size_t i;
    for (i = 0; i < s_rmt_probe_symbol_count; i++) {
        rmt_symbol_word_t *sym = &s_rmt_probe_symbols[i];
        uint32_t durs[2] = { sym->duration0, sym->duration1 };
        size_t j;
        for (j = 0; j < 2; j++) {
            uint32_t d = durs[j];
            if (d < RMT_PULSE_MIN_TICKS || d > RMT_PULSE_MAX_TICKS) {
                continue;
            }
            uint32_t idx = (d - RMT_PULSE_MIN_TICKS) / RMT_BUCKET_TICKS;
            if (idx >= RMT_BUCKET_COUNT) {
                idx = RMT_BUCKET_COUNT - 1;
            }
            histogram[idx]++;
            total_pulses++;
        }
    }
    if (total_pulses < 8) {
        return ESP_ERR_NOT_FOUND;
    }
    // 找直方图最高峰
    uint32_t best_bucket = 0;
    uint32_t best_count = 0;
    for (i = 0; i < RMT_BUCKET_COUNT; i++) {
        if (histogram[i] > best_count) {
            best_count = histogram[i];
            best_bucket = i;
        }
    }
    if (best_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    // 把"1 bit 时长"取为 bucket 中心
    uint32_t one_bit_ticks = RMT_PULSE_MIN_TICKS + best_bucket * RMT_BUCKET_TICKS + RMT_BUCKET_TICKS / 2;
    if (one_bit_ticks == 0) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    *out_baud = 1000000U / one_bit_ticks;
    if (*out_baud < 50) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    return ESP_OK;
}

// RMT 方案：替换原来的硬件 uart_detect_bitrate_start/stop。
// 流程：init RMT → 接收 ~800ms → 等待回调 → 解码出 baud → deinit RMT。
static esp_err_t app_uart_probe_detect_baudrate(uint32_t *out_baudrate)
{
    esp_err_t err;
    rmt_receive_config_t recv_cfg = {
        // ESP32 (original) RMT 滤波器硬件为 8-bit 计数器，按 APB 80MHz 计数。
        // 最大可设值 = 255 / 80 MHz ≈ 3187 ns。原 5000 ns 会触发 ESP_ERR_INVALID_ARG。
        // 关闭滤波器 (0) 对 baud 探测最友好：保留所有边沿，由 app_rmt_compute_baud 做统计过滤。
        .signal_range_min_ns = 0,                 // 关闭毛刺滤波
        // RMT 空闲阈值决定「多久没边沿就结束接收」。
        // 3600 bps 下 1 帧 ≈ 2.78ms；典型帧间空闲远 > 1ms，原 1ms 阈值会在第 1 帧前/后立刻结束。
        // ESP32 RMT_LL_MAX_IDLE_VALUE = 65535，1 MHz 分辨率下上限约 65.5ms。取 50ms 留余量。
        .signal_range_max_ns = 50000000,          // 50ms 空闲判定 = 1 帧 3600bps 数据 + 间隔
        .flags.en_partial_rx = false,
    };

    if (s_uart.driver_installed) {
        // RMT 探测期间必须关闭 UART 驱动，否则 RX 引脚被占用导致 RMT 收不到边沿。
        uart_driver_delete(UART_PORT);
        s_uart.driver_installed = false;
    }

    // 关键：UART 删除后，GPIO matrix 仍残留 RX 的 input routing (matrix_in 信号)。
    // 必须显式 reset pin 清掉旧路由，否则 RMT 通过 gpio_num 配置时可能拿到
    // 已被 UART 占用的 input 信号，看不到真实电平变化。
    gpio_reset_pin(app_uart_rx_gpio());

    // 给 GPIO matrix 一点时间稳定，否则 RMT 启动瞬间可能错过首字节。
    vTaskDelay(pdMS_TO_TICKS(20));

    app_uart_reset_probe_metrics();

    ESP_LOGI(TAG, "rmt probe start: rx_gpio=%d, idle_threshold=%d ns",
             (int)app_uart_rx_gpio(), (int)recv_cfg.signal_range_max_ns);

    err = app_rmt_probe_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "rmt_probe_init failed: 0x%x", (unsigned)err);
        app_uart_set_apply_error("rmt_init_failed");
        return err;
    }

    // 清空回调信号量（可能残留）
    xSemaphoreTake(s_rmt_probe_done_sem, 0);
    s_rmt_probe_symbol_count = 0;

    err = rmt_receive(s_rmt_probe_channel,
                      s_rmt_probe_symbols,
                      sizeof(s_rmt_probe_symbols),
                      &recv_cfg);
    if (err != ESP_OK) {
        // 把 esp_err_t 数值也带出来，方便定位是 INVALID_ARG / INVALID_STATE / FAIL。
        char err_msg[40];
        snprintf(err_msg, sizeof(err_msg), "rmt_receive_failed(0x%x)", (unsigned)err);
        app_rmt_probe_deinit();
        app_uart_set_apply_error(err_msg);
        return err;
    }

    // 等待接收完成（最长 UART_PROBE_TIME_MS = 800ms）+ 100ms 余量
    if (xSemaphoreTake(s_rmt_probe_done_sem, pdMS_TO_TICKS(UART_PROBE_TIME_MS + 100)) != pdTRUE) {
        // 超时：尝试取消接收
        rmt_receive(s_rmt_probe_channel, NULL, 0, NULL);  // 取消当前接收
        app_rmt_probe_deinit();
        app_uart_set_apply_error("rmt_probe_timeout");
        return ESP_ERR_TIMEOUT;
    }

    // 等待一小段时间让 ISR 完整写入 num_symbols
    vTaskDelay(pdMS_TO_TICKS(10));

    // 收尾：扫描 symbols 数组找第一个 duration0 == 0 && duration1 == 0 的位置作为末尾。
    // buffer 与 mem_block_symbols 保持一致（64），扫描范围同步。
    s_rmt_probe_symbol_count = 0;
    for (size_t i = 0; i < sizeof(s_rmt_probe_symbols) / sizeof(s_rmt_probe_symbols[0]); i++) {
        if (s_rmt_probe_symbols[i].duration0 == 0 && s_rmt_probe_symbols[i].duration1 == 0) {
            break;
        }
        s_rmt_probe_symbol_count++;
    }

    app_rmt_probe_deinit();

    // 诊断：打印收到的 symbol 数量 + 前 8 个的 duration 原始值，方便定位 RMT 看到的信号。
    ESP_LOGI(TAG, "rmt probe done: symbol_count=%u, first_durations=[%u,%u|%u,%u|%u,%u|%u,%u]",
             (unsigned)s_rmt_probe_symbol_count,
             (unsigned)s_rmt_probe_symbols[0].duration0, (unsigned)s_rmt_probe_symbols[0].duration1,
             (unsigned)s_rmt_probe_symbols[1].duration0, (unsigned)s_rmt_probe_symbols[1].duration1,
             (unsigned)s_rmt_probe_symbols[2].duration0, (unsigned)s_rmt_probe_symbols[2].duration1,
             (unsigned)s_rmt_probe_symbols[3].duration0, (unsigned)s_rmt_probe_symbols[3].duration1);

    if (s_rmt_probe_symbol_count < 4) {
        app_uart_set_apply_error("rmt_probe_no_data");
        return ESP_ERR_NOT_FOUND;
    }

    uint32_t baud = 0;
    err = app_rmt_compute_baud(&baud);
    if (err != ESP_OK) {
        app_uart_set_apply_error("rmt_probe_decode_failed");
        return err;
    }

    s_uart.raw_baudrate = baud;
    s_uart.probe_reliable = true;
    *out_baudrate = baud;
    return ESP_OK;
}


static void app_mqtt_build_uri(void)
{
    snprintf(s_mqtt.uri, sizeof(s_mqtt.uri), "%s://%s:%u",
             s_config.mqtt_use_tls ? "mqtts" : "mqtt",
             s_config.mqtt_host,
             s_config.mqtt_port);
}

static const char *app_mqtt_telemetry_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/telemetry";
    }
    return "iothub/telemetry";
}

static const char *app_mqtt_rpc_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/rpc/request/+";
    }
    return "iothub/rpc";
}

static const char *app_mqtt_attributes_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/attributes";
    }
    return "iothub/attributes";
}

static void app_mqtt_publish_attributes(void)
{
    char payload[384];
    char detected_frame[8];

    if (s_mqtt.client == NULL || !s_mqtt.connected) {
        return;
    }

    app_uart_build_frame(detected_frame, sizeof(detected_frame),
                         s_uart.detected_data_bits,
                         s_uart.detected_parity,
                         s_uart.detected_stop_bits);
    snprintf(payload, sizeof(payload),
             "{\"fw_version\":\"phase4\",\"net_mode\":\"%s\",\"ap_ssid\":\"%s\","
             "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"bt_mode\":\"%s\","
             "\"uart_frame\":\"%s\",\"uart_baudrate\":%u}",
             app_network_mode_to_string(s_config.net_mode),
             s_config.ap_ssid,
             s_config.sta_ssid,
             s_wifi.sta_ip,
             app_bt_mode_to_string(s_config.bt_mode),
             detected_frame,
             (unsigned)s_uart.detected_baudrate);

    esp_mqtt_client_publish(s_mqtt.client, app_mqtt_attributes_topic(), payload, 0, 1, 0);
}

static void app_mqtt_publish_state(void)
{
    char payload[512];
    char detected_frame[8];

    if (s_mqtt.client == NULL || !s_mqtt.connected) {
        return;
    }

    app_uart_capture_sample(0);
    app_uart_build_frame(detected_frame, sizeof(detected_frame),
                         s_uart.detected_data_bits,
                         s_uart.detected_parity,
                         s_uart.detected_stop_bits);
    snprintf(payload, sizeof(payload),
             "{\"relay_on\":%s,\"led_on\":%s,\"input_active\":%s,"
             "\"bt_mode\":\"%s\",\"net_mode\":\"%s\",\"sta_has_ip\":%s,"
             "\"free_heap\":%u,"
             "\"uart_baudrate\":%u,\"uart_frame\":\"%s\",\"uart_protocol\":\"%s\"}",
             s_config.relay_on ? "true" : "false",
             s_config.led_on ? "true" : "false",
             app_get_input_state() ? "true" : "false",
             app_bt_mode_to_string(s_config.bt_mode),
             app_network_mode_to_string(s_config.net_mode),
             s_wifi.sta_has_ip ? "true" : "false",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)s_uart.detected_baudrate,
             detected_frame,
             s_uart.protocol_hint);

    s_mqtt.last_msg_id = esp_mqtt_client_publish(s_mqtt.client, app_mqtt_telemetry_topic(),
                                                 payload, 0, 1, 0);
    if (s_mqtt.last_msg_id >= 0) {
        s_mqtt.publish_count++;
    }
}

static void app_mqtt_apply_command(const char *topic, const char *payload)
{
    bool handled = false;
    bool changed = false;
    bool request_status = false;
    char response_topic[128];
    char response_payload[256];
    const char *request_id = NULL;

    if (strstr(payload, "\"relay_on\":true") != NULL) {
        s_config.relay_on = true;
        changed = true;
        handled = true;
    } else if (strstr(payload, "\"relay_on\":false") != NULL) {
        s_config.relay_on = false;
        changed = true;
        handled = true;
    }

    if (strstr(payload, "\"led_on\":true") != NULL) {
        s_config.led_on = true;
        changed = true;
        handled = true;
    } else if (strstr(payload, "\"led_on\":false") != NULL) {
        s_config.led_on = false;
        changed = true;
        handled = true;
    }

    if (strstr(payload, "\"method\":\"setRelay\"") != NULL) {
        if (strstr(payload, "\"params\":true") != NULL || strstr(payload, "\"params\":1") != NULL) {
            s_config.relay_on = true;
        } else {
            s_config.relay_on = false;
        }
        changed = true;
        handled = true;
    }

    if (strstr(payload, "\"method\":\"setLed\"") != NULL) {
        if (strstr(payload, "\"params\":true") != NULL || strstr(payload, "\"params\":1") != NULL) {
            s_config.led_on = true;
        } else {
            s_config.led_on = false;
        }
        changed = true;
        handled = true;
    }

    if (strstr(payload, "\"method\":\"getStatus\"") != NULL) {
        handled = true;
        request_status = true;
    }

    if (changed) {
        app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
        app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
        app_save_config();
    }

    if (handled &&
        strncmp(topic, "v1/devices/me/rpc/request/", strlen("v1/devices/me/rpc/request/")) == 0) {
        request_id = topic + strlen("v1/devices/me/rpc/request/");
        snprintf(response_topic, sizeof(response_topic), "v1/devices/me/rpc/response/%s", request_id);
        if (request_status) {
            snprintf(response_payload, sizeof(response_payload),
                     "{\"relay_on\":%s,\"led_on\":%s,\"input_active\":%s,"
                     "\"net_mode\":\"%s\",\"sta_has_ip\":%s,\"sta_ip\":\"%s\"}",
                     s_config.relay_on ? "true" : "false",
                     s_config.led_on ? "true" : "false",
                     app_get_input_state() ? "true" : "false",
                     app_network_mode_to_string(s_config.net_mode),
                     s_wifi.sta_has_ip ? "true" : "false",
                     s_wifi.sta_ip);
        } else {
            snprintf(response_payload, sizeof(response_payload),
                     "{\"success\":true,\"relay_on\":%s,\"led_on\":%s}",
                     s_config.relay_on ? "true" : "false",
                     s_config.led_on ? "true" : "false");
        }
        esp_mqtt_client_publish(s_mqtt.client, response_topic, response_payload, 0, 1, 0);
    }

    if (handled) {
        app_mqtt_publish_attributes();
    }
    app_mqtt_publish_state();
}

static void app_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    char topic[96];
    char payload[192];
    int copy_len;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt.connected = true;
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "");
        esp_mqtt_client_subscribe(event->client, app_mqtt_rpc_topic(), 1);
        app_mqtt_publish_attributes();
        app_mqtt_publish_state();
        break;
    case MQTT_EVENT_DISCONNECTED:
        s_mqtt.connected = false;
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "disconnected");
        break;
    case MQTT_EVENT_PUBLISHED:
        s_mqtt.last_msg_id = event->msg_id;
        break;
    case MQTT_EVENT_DATA:
        copy_len = event->topic_len < (int)sizeof(topic) - 1 ? event->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, copy_len);
        topic[copy_len] = '\0';
        copy_len = event->data_len < (int)sizeof(payload) - 1 ? event->data_len : (int)sizeof(payload) - 1;
        memcpy(payload, event->data, copy_len);
        payload[copy_len] = '\0';
        app_mqtt_apply_command(topic, payload);
        break;
    case MQTT_EVENT_ERROR:
        s_mqtt.connected = false;
        snprintf(s_mqtt.last_error, sizeof(s_mqtt.last_error), "event_error_%ld", (long)event_id);
        break;
    default:
        break;
    }
}

static void app_mqtt_stop(void)
{
    if (s_mqtt.client != NULL) {
        esp_mqtt_client_stop(s_mqtt.client);
        esp_mqtt_client_destroy(s_mqtt.client);
        s_mqtt.client = NULL;
    }
    s_mqtt.connected = false;
}

static esp_err_t app_mqtt_apply_config(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};

    app_mqtt_stop();

    if (strlen(s_config.mqtt_host) == 0 || s_config.mqtt_port == 0) {
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "mqtt_not_configured");
        return ESP_OK;
    }
    if (!app_network_mode_has_sta(s_config.net_mode) || !s_wifi.sta_has_ip) {
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "waiting_sta_ip");
        return ESP_OK;
    }

    app_mqtt_build_uri();
    mqtt_cfg.broker.address.uri = s_mqtt.uri;
    mqtt_cfg.session.keepalive = 30;
    mqtt_cfg.network.timeout_ms = 5000;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;
    if (s_config.mqtt_use_tls) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (strlen(s_config.mqtt_token) > 0) {
        mqtt_cfg.credentials.username = s_config.mqtt_token;
    }

    s_mqtt.client = esp_mqtt_client_init(&mqtt_cfg);
    if (s_mqtt.client == NULL) {
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "mqtt_init_failed");
        return ESP_FAIL;
    }

    esp_mqtt_client_register_event(s_mqtt.client, ESP_EVENT_ANY_ID, app_mqtt_event_handler, NULL);
    ESP_RETURN_ON_ERROR(esp_mqtt_client_start(s_mqtt.client), TAG, "mqtt start failed");
    app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "connecting");
    return ESP_OK;
}

static void app_mqtt_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    if (app_mqtt_apply_config() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT apply config failed");
    }
    vTaskDelete(NULL);
}

static void app_mqtt_telemetry_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        app_mqtt_publish_state();
    }
}

static void app_reboot_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(APP_OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static esp_err_t device_info_get_handler(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    char mac_str[18];
    esp_chip_info_t chip_info;
    const esp_app_desc_t *app_desc = esp_app_get_description();

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    esp_chip_info(&chip_info);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);

    return http_send_jsonf(req, NULL,
                           "{\"project\":\"iothub\",\"idf_target\":\"%s\",\"idf_version\":\"%s\","
                           "\"app_version\":\"%s\",\"compile_date\":\"%s\",\"compile_time\":\"%s\","
                           "\"mac\":\"%s\",\"cpu_cores\":%d,\"free_heap\":%u}",
                           CONFIG_IDF_TARGET, esp_get_idf_version(), app_desc->version, app_desc->date,
                           app_desc->time, mac_str, chip_info.cores, (unsigned)esp_get_free_heap_size());
}

static esp_err_t device_status_get_handler(httpd_req_t *req)
{
    uint8_t mac[6] = {0};
    char mac_str[18];
    char detected_frame[8];
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

    esp_read_mac(mac, ESP_MAC_WIFI_SOFTAP);
    esp_chip_info(&chip_info);
    snprintf(mac_str, sizeof(mac_str), "%02X:%02X:%02X:%02X:%02X:%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    app_uart_build_frame(detected_frame, sizeof(detected_frame),
                         s_uart.detected_data_bits,
                         s_uart.detected_parity,
                         s_uart.detected_stop_bits);
    app_get_ap_network_strings(ip, sizeof(ip), gateway, sizeof(gateway), netmask, sizeof(netmask));
    app_get_running_partition_stats(&running_partition, &app_total, &app_used);
    app_get_storage_stats(&storage_total, &storage_used);

    return http_send_jsonf(req, NULL,
                           "{\"project\":\"iothub\",\"idf_target\":\"%s\",\"idf_version\":\"%s\","
                           "\"app_version\":\"%s\",\"mac\":\"%s\",\"cpu_cores\":%d,\"free_heap\":%u,"
                           "\"app_partition\":{\"label\":\"%s\",\"total_bytes\":%u,\"used_bytes\":%u,\"free_bytes\":%u},"
                           "\"storage\":{\"label\":\"storage\",\"total_bytes\":%u,\"used_bytes\":%u,\"free_bytes\":%u},"
                           "\"network\":{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_ip\":\"%s\","
                           "\"gateway\":\"%s\",\"netmask\":\"%s\",\"sta_ssid\":\"%s\","
                           "\"sta_connected\":%s,\"sta_has_ip\":%s,\"sta_ip\":\"%s\"},"
                           "\"gpio\":{\"relay_gpio\":%d,\"led_gpio\":%d,\"input_gpio\":%d,"
                           "\"relay_on\":%s,\"led_on\":%s,\"input_active\":%s},"
                           "\"bluetooth\":{\"mode\":\"%s\",\"device_name\":\"%s\",\"runtime_mode\":\"%s\"},"
                           "\"mqtt\":{\"backend\":\"%s\",\"host\":\"%s\",\"port\":%u,\"use_tls\":%s,"
                           "\"connected\":%s,\"last_error\":\"%s\",\"publish_count\":%u},"
                           "\"uart\":{\"rx_gpio\":%ld,\"tx_gpio\":%ld,\"baudrate\":%u,\"frame\":\"%s\","
                           "\"protocol_hint\":\"%s\",\"sample\":\"%s\"}}",
                           CONFIG_IDF_TARGET, esp_get_idf_version(), app_desc->version, mac_str,
                           chip_info.cores, (unsigned)esp_get_free_heap_size(),
                           running_partition != NULL ? running_partition->label : "unknown",
                           (unsigned)app_total,
                           (unsigned)app_used,
                           (unsigned)(app_total > app_used ? (app_total - app_used) : 0),
                           (unsigned)storage_total,
                           (unsigned)storage_used,
                           (unsigned)(storage_total > storage_used ? (storage_total - storage_used) : 0),
                           app_network_mode_to_string(s_config.net_mode),
                           s_config.ap_ssid, ip, gateway, netmask, s_config.sta_ssid,
                           s_wifi.sta_connected ? "true" : "false",
                           s_wifi.sta_has_ip ? "true" : "false",
                           s_wifi.sta_ip,
                           app_relay_gpio(), app_led_gpio(), app_input_gpio(),
                           s_config.relay_on ? "true" : "false",
                           s_config.led_on ? "true" : "false",
                           app_get_input_state() ? "true" : "false",
                           app_bt_mode_to_string(s_config.bt_mode), s_config.bt_device_name,
                           app_bt_mode_to_string(s_bt_runtime_mode), s_config.mqtt_backend,
                           s_config.mqtt_host, s_config.mqtt_port,
                           s_config.mqtt_use_tls ? "true" : "false",
                           s_mqtt.connected ? "true" : "false",
                           s_mqtt.last_error,
                           s_mqtt.publish_count,
                           (long)app_uart_rx_gpio(),
                           (long)app_uart_tx_gpio(),
                           s_uart.detected_baudrate,
                           detected_frame,
                           s_uart.protocol_hint,
                           s_uart.last_ascii);
}

static esp_err_t network_get_handler(httpd_req_t *req)
{
    char ip[16];
    char gateway[16];
    char netmask[16];

    app_get_ap_network_strings(ip, sizeof(ip), gateway, sizeof(gateway), netmask, sizeof(netmask));
    return http_send_jsonf(req, NULL,
                           "{\"mode\":\"%s\",\"ap_ssid\":\"%s\",\"ap_password\":\"%s\","
                           "\"ap_ip\":\"%s\",\"gateway\":\"%s\",\"netmask\":\"%s\","
                           "\"password_enabled\":%s,"
                           "\"sta_ssid\":\"%s\",\"sta_password\":\"%s\","
                           "\"sta_connected\":%s,\"sta_has_ip\":%s,"
                           "\"sta_ip\":\"%s\",\"sta_gateway\":\"%s\",\"sta_netmask\":\"%s\","
                           "\"last_disconnect\":\"%s\"}",
                           app_network_mode_to_string(s_config.net_mode),
                           s_config.ap_ssid, s_config.ap_password, ip, gateway, netmask,
                           strlen(s_config.ap_password) > 0 ? "true" : "false",
                           s_config.sta_ssid, s_config.sta_password,
                           s_wifi.sta_connected ? "true" : "false",
                           s_wifi.sta_has_ip ? "true" : "false",
                           s_wifi.sta_ip, s_wifi.sta_gateway, s_wifi.sta_netmask,
                           s_wifi.last_disconnect);
}

static esp_err_t network_put_handler(httpd_req_t *req)
{
    char value[65];
    const bool ap_enabled_before = app_network_mode_has_ap(s_config.net_mode);
    char current_ap_ip[16];
    char current_ap_gw[16];
    char current_ap_mask[16];

    app_get_ap_network_strings(current_ap_ip, sizeof(current_ap_ip),
                               current_ap_gw, sizeof(current_ap_gw),
                               current_ap_mask, sizeof(current_ap_mask));

    if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (json_find_string(s_web.scratch, "ap_ssid", value, sizeof(s_config.ap_ssid)) && strlen(value) > 0) {
        app_copy_string(s_config.ap_ssid, sizeof(s_config.ap_ssid), value);
    }
    if (json_find_string(s_web.scratch, "mode", value, sizeof(value))) {
        s_config.net_mode = app_network_mode_from_string(value);
    }
    if (json_find_string(s_web.scratch, "ap_password", value, sizeof(s_config.ap_password))) {
        if (strlen(value) > 0 && strlen(value) < 8) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"password_too_short\"}");
        }
        app_copy_string(s_config.ap_password, sizeof(s_config.ap_password), value);
    }
    if (json_find_string(s_web.scratch, "sta_ssid", value, sizeof(s_config.sta_ssid))) {
        app_copy_string(s_config.sta_ssid, sizeof(s_config.sta_ssid), value);
    }
    if (json_find_string(s_web.scratch, "sta_password", value, sizeof(s_config.sta_password))) {
        if (strlen(value) > 0 && strlen(value) < 8) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"sta_password_too_short\"}");
        }
        app_copy_string(s_config.sta_password, sizeof(s_config.sta_password), value);
    }

    if (app_save_config() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    return http_send_jsonf(req, NULL,
                           "{\"status\":\"saved\",\"message\":\"network config saved\","
                           "\"restart_required\":true,\"ap_enabled\":%s,"
                           "\"next_access_ip\":\"%s\",\"current_ap_ip\":\"%s\","
                           "\"mode\":\"%s\",\"sta_ssid\":\"%s\"}",
                           app_network_mode_has_ap(s_config.net_mode) ? "true" : "false",
                           app_network_mode_has_ap(s_config.net_mode) ? "192.168.8.1" : "",
                           ap_enabled_before ? current_ap_ip : "",
                           app_network_mode_to_string(s_config.net_mode),
                           s_config.sta_ssid);
}

static esp_err_t network_restart_handler(httpd_req_t *req)
{
    (void)req;
    http_send_json_text(req, NULL,
                        "{\"status\":\"restarting\",\"message\":\"network restart started\"}");
    xTaskCreate(app_wifi_restart_task, "wifi_restart", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t network_scan_handler(httpd_req_t *req)
{
    char *payload;
    int offset;
    uint16_t index;

    if (app_wifi_perform_scan() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"wifi_scan_failed\"}");
    }

    payload = malloc(APP_JSON_BUFFER_SIZE);
    if (payload == NULL) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    offset = snprintf(payload, APP_JSON_BUFFER_SIZE, "{\"status\":\"ok\",\"count\":%u,\"aps\":[",
                      s_wifi.scan_count);
    for (index = 0; index < s_wifi.scan_count && offset < APP_JSON_BUFFER_SIZE - 96; index++) {
        offset += snprintf(payload + offset, APP_JSON_BUFFER_SIZE - offset,
                           "%s{\"ssid\":\"%s\",\"rssi\":%d,\"auth\":\"%s\"}",
                           index == 0 ? "" : ",",
                           (char *)s_wifi.scan_records[index].ssid,
                           s_wifi.scan_records[index].rssi,
                           app_wifi_authmode_to_string(s_wifi.scan_records[index].authmode));
    }
    snprintf(payload + offset, APP_JSON_BUFFER_SIZE - offset, "]}");
    http_send_json_text(req, NULL, payload);
    free(payload);
    return ESP_OK;
}

static esp_err_t gpio_get_handler(httpd_req_t *req)
{
    char *payload = malloc(APP_JSON_BUFFER_SIZE);
    size_t offset = 0;

    if (payload == NULL) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    offset += snprintf(payload + offset, APP_JSON_BUFFER_SIZE - offset,
                       "{\"relay_gpio\":%ld,\"led_gpio\":%ld,\"input_gpio\":%ld,"
                       "\"relay_on\":%s,\"led_on\":%s,\"input_active\":%s,"
                       "\"relay_active_high\":%s,\"led_active_high\":%s,\"input_active_low\":%s,"
                       "\"uart_rx_gpio\":%ld,\"uart_tx_gpio\":%ld",
                       (long)app_relay_gpio(),
                       (long)app_led_gpio(),
                       (long)app_input_gpio(),
                       s_config.relay_on ? "true" : "false",
                       s_config.led_on ? "true" : "false",
                       app_get_input_state() ? "true" : "false",
                       s_config.relay_active_high ? "true" : "false",
                       s_config.led_active_high ? "true" : "false",
                       s_config.input_active_low ? "true" : "false",
                       (long)app_uart_rx_gpio(),
                       (long)app_uart_tx_gpio());
    snprintf(payload + offset, APP_JSON_BUFFER_SIZE - offset, "}");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    httpd_resp_sendstr(req, payload);
    free(payload);
    return ESP_OK;
}

static esp_err_t gpio_put_handler(httpd_req_t *req)
{
    bool value;

    if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (json_find_bool(s_web.scratch, "relay_on", &value)) {
        s_config.relay_on = value;
        app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
    }
    if (json_find_bool(s_web.scratch, "led_on", &value)) {
        s_config.led_on = value;
        app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
    }

    app_mqtt_publish_state();

    if (app_save_config() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    return gpio_get_handler(req);
}

static esp_err_t bluetooth_get_handler(httpd_req_t *req)
{
    return http_send_jsonf(req, NULL,
                           "{\"mode\":\"%s\",\"device_name\":\"%s\",\"runtime_mode\":\"%s\","
                           "\"last_error\":\"%s\"}",
                           app_bt_mode_to_string(s_config.bt_mode),
                           s_config.bt_device_name,
                           app_bt_mode_to_string(s_bt_runtime_mode),
                           s_bt_last_error);
}

static esp_err_t bluetooth_put_handler(httpd_req_t *req)
{
    char value[64];

    if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (json_find_string(s_web.scratch, "mode", value, sizeof(value))) {
        s_config.bt_mode = app_bt_mode_from_string(value);
    }
    if (json_find_string(s_web.scratch, "device_name", value, sizeof(s_config.bt_device_name)) &&
        strlen(value) > 0) {
        app_copy_string(s_config.bt_device_name, sizeof(s_config.bt_device_name), value);
    }

    if (app_save_config() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"bluetooth config saved, mode will restart\"}");
    xTaskCreate(app_bt_restart_task, "bt_restart", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t mqtt_get_handler(httpd_req_t *req)
{
    app_uart_capture_sample(0);
    return http_send_jsonf(req, NULL,
                           "{\"backend\":\"%s\",\"host\":\"%s\",\"port\":%u,\"token\":\"%s\","
                           "\"use_tls\":%s,\"connected\":%s,\"uri\":\"%s\",\"last_error\":\"%s\","
                           "\"publish_count\":%u}",
                           s_config.mqtt_backend, s_config.mqtt_host, s_config.mqtt_port,
                           s_config.mqtt_token, s_config.mqtt_use_tls ? "true" : "false",
                           s_mqtt.connected ? "true" : "false",
                           s_mqtt.uri,
                           s_mqtt.last_error,
                           s_mqtt.publish_count);
}

static esp_err_t mqtt_put_handler(httpd_req_t *req)
{
    char value[128];
    bool bool_value;
    uint16_t port;

    if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (json_find_string(s_web.scratch, "backend", value, sizeof(s_config.mqtt_backend)) && strlen(value) > 0) {
        app_copy_string(s_config.mqtt_backend, sizeof(s_config.mqtt_backend), value);
    }
    if (json_find_string(s_web.scratch, "host", value, sizeof(s_config.mqtt_host)) && strlen(value) > 0) {
        app_copy_string(s_config.mqtt_host, sizeof(s_config.mqtt_host), value);
    }
    if (json_find_string(s_web.scratch, "token", value, sizeof(s_config.mqtt_token))) {
        app_copy_string(s_config.mqtt_token, sizeof(s_config.mqtt_token), value);
    }
    if (json_find_u16(s_web.scratch, "port", &port) && port > 0) {
        s_config.mqtt_port = port;
    }
    if (json_find_bool(s_web.scratch, "use_tls", &bool_value)) {
        s_config.mqtt_use_tls = bool_value;
    }

    if (app_save_config() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    http_send_json_text(req, NULL,
                        "{\"status\":\"saved\",\"message\":\"mqtt config saved, connection will restart\"}");
    xTaskCreate(app_mqtt_restart_task, "mqtt_restart", 4096, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t uart_get_handler(httpd_req_t *req)
{
    app_uart_capture_sample(0);
    return app_uart_send_runtime_json(req, "uart_runtime");
}

static esp_err_t uart_put_handler(httpd_req_t *req)
{
    char value[64];
    uint32_t baudrate;
    uint16_t short_value;
    const char *error = NULL;

    if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (json_find_u32(s_web.scratch, "baudrate", &baudrate) && baudrate > 0) {
        s_config.uart_baudrate = baudrate;
    }
    if (json_find_string(s_web.scratch, "parity", value, sizeof(value))) {
        s_config.uart_parity_mode = app_uart_parity_from_string(value);
    }
    if (json_find_u16(s_web.scratch, "data_bits", &short_value)) {
        if (!app_uart_data_bits_valid((uint8_t)short_value)) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"invalid_uart_data_bits\"}");
        }
        s_config.uart_data_bits = (uint8_t)short_value;
    }
    if (json_find_u16(s_web.scratch, "stop_bits", &short_value)) {
        if (!app_uart_stop_bits_valid((uint8_t)short_value)) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"invalid_uart_stop_bits\"}");
        }
        s_config.uart_stop_bits = (uint8_t)short_value;
    }

    if (!app_validate_fixed_gpio_config(&error)) {
        return http_send_jsonf(req, "400 Bad Request",
                               "{\"status\":\"error\",\"message\":\"%s\"}",
                               error != NULL ? error : "gpio_uart_conflict");
    }

    if (app_save_config() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    if (app_uart_apply_config() != ESP_OK) {
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"%s\"}",
                               app_uart_get_apply_error());
    }
    s_uart.probe_pending_confirmation = false;
    s_uart.probe_confirmed = false;
    s_uart.pending_baudrate = 0;
    s_uart.pending_data_bits = s_config.uart_data_bits;
    s_uart.pending_stop_bits = s_config.uart_stop_bits;
    s_uart.pending_parity = UART_PARITY_DISABLE;
    // 用户已主动改 saved config，探测中遗留的 snapshot 失去意义。
    s_uart.restore_snapshot_valid = false;
    app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "idle");
    app_uart_reset_manual_session();
    return uart_get_handler(req);
}

static esp_err_t uart_probe_auto_detect_handler(httpd_req_t *req)
{
    uint32_t detected_baudrate = 0;
    int score;
    TickType_t probe_ticks;
    char apply_error[48];

    if (req->content_len > 0) {
        if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    // 进入探测前先把"当时正在运行的配置"存到 snapshot。
    // 注意：这里捕获的是 s_config（已保存的真实配置），不是页面当前填写值。
    app_uart_capture_snapshot();
    // 探测会话固定 8N1 - 与 saved config / 页面值完全无关。
    s_uart.probe_session.baudrate = 0; // 由 uart_detect_bitrate 测出后填入
    s_uart.probe_session.data_bits = 8;
    s_uart.probe_session.stop_bits = 1;
    s_uart.probe_session.parity = UART_PARITY_DISABLE;

    if (app_uart_probe_detect_baudrate(&detected_baudrate) != ESP_OK) {
        // 探测失败：仅恢复 running config，不修改 s_config。
        char apply_error[48];
        app_copy_string(apply_error, sizeof(apply_error), app_uart_get_apply_error());
        if (app_uart_restore_from_snapshot() == ESP_OK) {
            app_uart_capture_sample(pdMS_TO_TICKS(100));
        }
        return http_send_jsonf(req, "404 Not Found",
                               "{\"status\":\"error\",\"message\":\"uart_probe_failed\",\"detail\":\"%s\"}",
                               apply_error);
    }

    // 试跑：使用探测会话（detected_baudrate + 8N1），而不是 s_config。
    s_uart.probe_session.baudrate = detected_baudrate;
    if (app_uart_install_driver(detected_baudrate, 8, UART_PARITY_DISABLE, 1) != ESP_OK) {
        app_copy_string(apply_error, sizeof(apply_error), app_uart_get_apply_error());
        if (app_uart_restore_from_snapshot() != ESP_OK) {
            // 恢复也失败，返回 uart_restore_failed，告知前端不要继续探测。
            return http_send_json_text(req, "500 Internal Server Error",
                                       "{\"status\":\"error\",\"message\":\"uart_restore_failed\"}");
        }
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"probe_apply_failed\",\"detail\":\"%s\"}",
                               apply_error);
    }

    probe_ticks = pdMS_TO_TICKS(detected_baudrate <= UART_LOW_BAUD_THRESHOLD
                                    ? UART_LOW_BAUD_PROBE_TIME_MS
                                    : UART_PARITY_PROBE_TIME_MS);
    app_uart_capture_sample(probe_ticks);
    score = app_uart_probe_score_current();
    s_uart.detected_baudrate = detected_baudrate;
    s_uart.detected_data_bits = 8;
    s_uart.detected_stop_bits = 1;
    s_uart.detected_parity = UART_PARITY_DISABLE;
    s_uart.pending_baudrate = detected_baudrate;
    s_uart.pending_data_bits = 8;
    s_uart.pending_stop_bits = 1;
    s_uart.pending_parity = UART_PARITY_DISABLE;
    s_uart.probe_pending_confirmation = true;
    s_uart.probe_confirmed = false;
    s_uart.probe_best_score = score;
    s_uart.probe_confirm_score = score;
    app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "auto");
    app_uart_store_probe_result(detected_baudrate, UART_PARITY_DISABLE, score);
    return app_uart_send_runtime_json(req, "auto_detect_ready");
}

static esp_err_t uart_probe_manual_detect_handler(httpd_req_t *req)
{
    char action[16] = "next";
    char apply_error[48];
    uint32_t preferred_baudrate;
    uint32_t exclude_baudrate;
    uart_parity_t exclude_parity;
    uart_probe_candidate_t candidate;
    TickType_t probe_ticks;
    int score;
    bool start_session;

    if (req->content_len > 0) {
        if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
            return ESP_FAIL;
        }
        json_find_string(s_web.scratch, "action", action, sizeof(action));
    }

    start_session = strcmp(action, "start") == 0 || !s_uart.manual_session_active;
    if (start_session) {
        // 手动轮询也用独立探测会话。绝对不能读 s_config.uart_baudrate。
        // 起点优先选上一次 pending_baudrate（自动探测给到的结果），
        // 否则用 raw_baudrate 归一化后的常用值，最后才用 s_uart.probe_session.baudrate。
        app_uart_reset_manual_session();
        app_uart_capture_snapshot();
        // 探测会话：手动轮询固定 8x1 + parity。
        s_uart.probe_session.data_bits = 8;
        s_uart.probe_session.stop_bits = 1;
        if (s_uart.pending_baudrate != 0) {
            preferred_baudrate = s_uart.pending_baudrate;
        } else if (s_uart.raw_baudrate > 0) {
            preferred_baudrate = app_uart_normalize_baudrate(s_uart.raw_baudrate);
        } else {
            // 没有 raw_baudrate 时使用最常用的 9600 作为占位起点。
            preferred_baudrate = 9600;
        }
        s_uart.probe_session.baudrate = preferred_baudrate;
        exclude_baudrate = s_uart.pending_baudrate;
        exclude_parity = s_uart.pending_parity;
        s_uart.manual_candidate_count = app_uart_build_manual_candidate_list(preferred_baudrate,
                                                                             s_uart.raw_baudrate,
                                                                             s_uart.probe_reliable,
                                                                             exclude_baudrate,
                                                                             exclude_parity,
                                                                             s_uart.manual_candidates,
                                                                             sizeof(s_uart.manual_candidates) /
                                                                                 sizeof(s_uart.manual_candidates[0]));
        s_uart.manual_session_active = s_uart.manual_candidate_count > 0;
        if (!s_uart.manual_session_active) {
            // 列表为空：清掉无效的 snapshot，避免污染后续恢复。
            s_uart.restore_snapshot_valid = false;
            return http_send_json_text(req, "404 Not Found",
                                       "{\"status\":\"error\",\"message\":\"manual_probe_unavailable\"}");
        }
    }

    if (!s_uart.manual_session_active || s_uart.manual_candidate_index >= s_uart.manual_candidate_count) {
        s_uart.probe_pending_confirmation = false;
        s_uart.pending_baudrate = 0;
        s_uart.pending_data_bits = 8;
        s_uart.pending_stop_bits = 1;
        s_uart.pending_parity = UART_PARITY_DISABLE;
        app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "idle");
        app_uart_reset_manual_session();
        // 候选耗尽：用 snapshot 还原 running config。
        if (app_uart_restore_from_snapshot() == ESP_OK) {
            app_uart_capture_sample(pdMS_TO_TICKS(100));
            return http_send_json_text(req, "404 Not Found",
                                       "{\"status\":\"error\",\"message\":\"all_candidates_exhausted\"}");
        }
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"uart_restore_failed\"}");
    }

    candidate = s_uart.manual_candidates[s_uart.manual_candidate_index];
    s_uart.manual_candidate_index++;
    if (app_uart_install_driver(candidate.baudrate, 8, candidate.parity, 1) != ESP_OK) {
        app_copy_string(apply_error, sizeof(apply_error), app_uart_get_apply_error());
        if (app_uart_restore_from_snapshot() != ESP_OK) {
            return http_send_json_text(req, "500 Internal Server Error",
                                       "{\"status\":\"error\",\"message\":\"uart_restore_failed\"}");
        }
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"probe_apply_failed\",\"detail\":\"%s\"}",
                               apply_error);
    }

    probe_ticks = pdMS_TO_TICKS(candidate.baudrate <= UART_LOW_BAUD_THRESHOLD
                                    ? UART_LOW_BAUD_PROBE_TIME_MS
                                    : UART_PARITY_PROBE_TIME_MS);
    app_uart_capture_sample(probe_ticks);
    score = app_uart_probe_score_current();
    s_uart.detected_baudrate = candidate.baudrate;
    s_uart.detected_data_bits = 8;
    s_uart.detected_stop_bits = 1;
    s_uart.detected_parity = candidate.parity;
    s_uart.pending_baudrate = candidate.baudrate;
    s_uart.pending_data_bits = 8;
    s_uart.pending_stop_bits = 1;
    s_uart.pending_parity = candidate.parity;
    s_uart.probe_pending_confirmation = true;
    s_uart.probe_confirmed = false;
    s_uart.probe_best_score = score;
    s_uart.probe_confirm_score = score;
    app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "manual");
    app_uart_store_probe_result(candidate.baudrate, candidate.parity, score);
    return app_uart_send_runtime_json(req, "manual_detect_ready");
}

static esp_err_t uart_probe_confirm_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    if (!s_uart.probe_pending_confirmation || s_uart.pending_baudrate == 0) {
        return http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"uart_probe_not_pending\"}");
    }

    s_config.uart_baudrate = s_uart.pending_baudrate;
    s_config.uart_data_bits = s_uart.pending_data_bits;
    s_config.uart_stop_bits = s_uart.pending_stop_bits;
    s_config.uart_parity_mode = s_uart.pending_parity;
    if (app_save_config() != ESP_OK) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }
    if (app_uart_apply_config() != ESP_OK) {
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"%s\"}",
                               app_uart_get_apply_error());
    }

    app_uart_capture_sample(pdMS_TO_TICKS(100));
    s_uart.probe_pending_confirmation = false;
    s_uart.probe_confirmed = true;
    // confirm 成功：探测已结束，snapshot 不再有效，避免被误用。
    s_uart.restore_snapshot_valid = false;
    app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "idle");
    app_uart_reset_manual_session();
    return app_uart_send_runtime_json(req, "uart_probe_saved");
}

static esp_err_t uart_probe_stop_handler(httpd_req_t *req)
{
    if (req->content_len > 0) {
        if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
            return ESP_FAIL;
        }
    }

    s_uart.probe_pending_confirmation = false;
    s_uart.probe_confirmed = false;
    s_uart.pending_baudrate = 0;
    s_uart.pending_data_bits = 8;
    s_uart.pending_stop_bits = 1;
    s_uart.pending_parity = UART_PARITY_DISABLE;
    app_copy_string(s_uart.probe_mode, sizeof(s_uart.probe_mode), "idle");
    app_uart_reset_manual_session();
    // stop：用 snapshot 还原 running config，而不是重新读 s_config 重新应用。
    if (app_uart_restore_from_snapshot() != ESP_OK) {
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"%s\"}",
                               app_uart_get_apply_error());
    }
    app_uart_capture_sample(pdMS_TO_TICKS(100));
    return app_uart_send_runtime_json(req, "uart_probe_stopped");
}

static bool app_uart_parse_hex_bytes(const char *text, uint8_t *out, size_t out_size, size_t *out_len)
{
    size_t count = 0;
    int high_nibble = -1;

    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text++;
        int value;

        if (isspace(ch) || ch == ',' || ch == ';' || ch == '-') {
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            value = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            value = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            value = ch - 'A' + 10;
        } else {
            return false;
        }

        if (high_nibble < 0) {
            high_nibble = value;
            continue;
        }

        if (count >= out_size) {
            return false;
        }
        out[count++] = (uint8_t)((high_nibble << 4) | value);
        high_nibble = -1;
    }

    if (high_nibble >= 0) {
        return false;
    }

    if (out_len != NULL) {
        *out_len = count;
    }
    return true;
}

static esp_err_t uart_console_get_handler(httpd_req_t *req)
{
    char *plain_json = NULL;
    char *hex_json = NULL;
    esp_err_t err;
    bool locked = false;

    app_uart_poll_console(0);
    plain_json = malloc(UART_CONSOLE_PLAIN_SIZE * 2 + 1);
    hex_json = malloc(UART_CONSOLE_HEX_SIZE * 2 + 1);
    if (plain_json == NULL || hex_json == NULL) {
        free(plain_json);
        free(hex_json);
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    // 复制到本地堆时也要加锁，避免与 capture_sample 的写并发导致撕裂。
    if (s_uart_console_mutex != NULL &&
        xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(100)) == pdTRUE) {
        locked = true;
        app_json_escape_string(plain_json, UART_CONSOLE_PLAIN_SIZE * 2 + 1, s_uart.rx_plain);
        app_json_escape_string(hex_json, UART_CONSOLE_HEX_SIZE * 2 + 1, s_uart.rx_hex);
    } else {
        app_json_escape_string(plain_json, UART_CONSOLE_PLAIN_SIZE * 2 + 1, s_uart.rx_plain);
        app_json_escape_string(hex_json, UART_CONSOLE_HEX_SIZE * 2 + 1, s_uart.rx_hex);
    }

    err = http_send_jsonf(req, NULL,
                          "{\"plain\":\"%s\",\"hex\":\"%s\",\"rx_sequence\":%u}",
                          plain_json,
                          hex_json,
                          (unsigned)s_uart.rx_sequence);
    if (locked) {
        xSemaphoreGiveRecursive(s_uart_console_mutex);
    }
    free(plain_json);
    free(hex_json);
    return err;
}

static esp_err_t uart_send_handler(httpd_req_t *req)
{
    char data[UART_TX_BUFFER_SIZE];
    char encoding[8] = "plain";
    uint8_t tx_bytes[UART_TX_BUFFER_SIZE / 2];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    int written;

    if (!s_uart.driver_installed) {
        return http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_not_ready\"}");
    }
    if (http_read_body(req, s_web.scratch, sizeof(s_web.scratch)) != ESP_OK) {
        return ESP_FAIL;
    }

    if (!json_find_string(s_web.scratch, "data", data, sizeof(data))) {
        return http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"uart_data_required\"}");
    }
    json_find_string(s_web.scratch, "encoding", encoding, sizeof(encoding));

    if (strcmp(encoding, "hex") == 0) {
        if (!app_uart_parse_hex_bytes(data, tx_bytes, sizeof(tx_bytes), &payload_len) || payload_len == 0) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"uart_hex_invalid\"}");
        }
        payload = tx_bytes;
    } else {
        payload = (const uint8_t *)data;
        payload_len = strlen(data);
        if (payload_len == 0) {
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"uart_data_required\"}");
        }
    }

    written = uart_write_bytes(UART_PORT, payload, payload_len);
    if (written < 0) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"uart_send_failed\"}");
    }

    return http_send_jsonf(req, NULL,
                           "{\"status\":\"ok\",\"bytes_sent\":%u}",
                           (unsigned)written);
}

static esp_err_t ota_upload_handler(httpd_req_t *req)
{
    const esp_partition_t *update_partition;
    esp_ota_handle_t update_handle = 0;
    int remaining = req->content_len;
    esp_err_t err;

    if (remaining <= 0) {
        return http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"empty_upload\"}");
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        return http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"no_update_partition\"}");
    }

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"esp_ota_begin_failed:%s\"}",
                               esp_err_to_name(err));
    }

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, s_web.scratch,
                                      remaining > APP_SCRATCH_SIZE ? APP_SCRATCH_SIZE : remaining);
        if (recv_len <= 0) {
            esp_ota_abort(update_handle);
            return http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"ota_recv_failed\"}");
        }
        err = esp_ota_write(update_handle, s_web.scratch, recv_len);
        if (err != ESP_OK) {
            esp_ota_abort(update_handle);
            return http_send_jsonf(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"esp_ota_write_failed:%s\"}",
                                   esp_err_to_name(err));
        }
        remaining -= recv_len;
    }

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        esp_ota_abort(update_handle);
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"esp_ota_end_failed:%s\"}",
                               esp_err_to_name(err));
    }

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        return http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"set_boot_partition_failed:%s\"}",
                               esp_err_to_name(err));
    }

    http_send_json_text(req, NULL,
                        "{\"status\":\"ok\",\"message\":\"ota image received, device will reboot\"}");
    xTaskCreate(app_reboot_task, "ota_reboot", 2048, NULL, 5, NULL);
    return ESP_OK;
}

static esp_err_t index_handler(httpd_req_t *req)
{
    return http_serve_index(req);
}

static void app_start_webserver(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.uri_match_fn = httpd_uri_match_wildcard;
    config.max_uri_handlers = 28;
    ESP_ERROR_CHECK(httpd_start(&s_web.server, &config));

    const httpd_uri_t uris[] = {
        {.uri = "/api/v1/device/info", .method = HTTP_GET, .handler = device_info_get_handler},
        {.uri = "/api/v1/device/status", .method = HTTP_GET, .handler = device_status_get_handler},
        {.uri = "/api/v1/config/network", .method = HTTP_GET, .handler = network_get_handler},
        {.uri = "/api/v1/config/network", .method = HTTP_PUT, .handler = network_put_handler},
        {.uri = "/api/v1/network/restart", .method = HTTP_POST, .handler = network_restart_handler},
        {.uri = "/api/v1/network/scan", .method = HTTP_GET, .handler = network_scan_handler},
        {.uri = "/api/v1/config/gpio", .method = HTTP_GET, .handler = gpio_get_handler},
        {.uri = "/api/v1/config/gpio", .method = HTTP_PUT, .handler = gpio_put_handler},
        {.uri = "/api/v1/config/bluetooth", .method = HTTP_GET, .handler = bluetooth_get_handler},
        {.uri = "/api/v1/config/bluetooth", .method = HTTP_PUT, .handler = bluetooth_put_handler},
        {.uri = "/api/v1/config/mqtt", .method = HTTP_GET, .handler = mqtt_get_handler},
        {.uri = "/api/v1/config/mqtt", .method = HTTP_PUT, .handler = mqtt_put_handler},
        {.uri = "/api/v1/config/uart", .method = HTTP_GET, .handler = uart_get_handler},
        {.uri = "/api/v1/config/uart", .method = HTTP_PUT, .handler = uart_put_handler},
        {.uri = "/api/v1/uart/probe", .method = HTTP_GET, .handler = uart_get_handler},
        {.uri = "/api/v1/uart/probe/auto_detect", .method = HTTP_POST, .handler = uart_probe_auto_detect_handler},
        {.uri = "/api/v1/uart/probe/manual_detect", .method = HTTP_POST, .handler = uart_probe_manual_detect_handler},
        {.uri = "/api/v1/uart/probe/confirm", .method = HTTP_POST, .handler = uart_probe_confirm_handler},
        {.uri = "/api/v1/uart/probe/stop", .method = HTTP_POST, .handler = uart_probe_stop_handler},
        {.uri = "/api/v1/uart/console", .method = HTTP_GET, .handler = uart_console_get_handler},
        {.uri = "/api/v1/uart/send", .method = HTTP_POST, .handler = uart_send_handler},
        {.uri = "/api/v1/ota/upload", .method = HTTP_POST, .handler = ota_upload_handler},
        {.uri = "/*", .method = HTTP_GET, .handler = index_handler},
    };
    size_t index;

    for (index = 0; index < sizeof(uris) / sizeof(uris[0]); index++) {
        ESP_ERROR_CHECK(httpd_register_uri_handler(s_web.server, &uris[index]));
    }
}

void app_main(void)
{
    // 必须在最早期初始化：MQTT 遥测 task 启动后就会调用 app_uart_capture_sample。
    s_uart_console_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_uart_console_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create uart console mutex");
    }

    esp_err_t err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(err);

    app_load_config();
    app_configure_gpio();
    ESP_ERROR_CHECK(app_mount_fs());
    app_start_wifi();
    if (app_bt_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "Bluetooth init skipped");
    }
    if (app_uart_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "UART init skipped");
    }
    if (app_mqtt_apply_config() != ESP_OK) {
        ESP_LOGW(TAG, "MQTT init skipped");
    }
    xTaskCreate(app_mqtt_telemetry_task, "mqtt_telemetry", 4096, NULL, 5, NULL);
    app_start_webserver();

    ESP_LOGI(TAG, "iothub phase-4 ready: mode=%s, AP SSID='%s', open http://192.168.8.1 when AP is enabled",
             app_network_mode_to_string(s_config.net_mode),
             s_config.ap_ssid);
}
