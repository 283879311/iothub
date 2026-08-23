#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "common.h"
#include "config.h"
#include "http_utils.h"
#include "state.h"
#include "uart.h"
#include "uart_console.h"
#include "uart_internal.h"

static const char *TAG = "uart";

extern app_config_t s_config;

uart_runtime_t s_uart = {0};
static SemaphoreHandle_t s_uart_op_mutex = NULL;
static SemaphoreHandle_t s_json_mutex = NULL;
static TaskHandle_t s_uart_keepalive_task = NULL;

static char s_escaped_tx_data[UART_TX_BUFFER_SIZE * 2];
static char s_escaped_tx_preview[UART_TX_PREVIEW_SIZE * 2];
static char s_escaped_tx_hex[UART_TX_HEX_TEXT_SIZE * 2];
static char s_escaped_keepalive_data[UART_TX_BUFFER_SIZE * 2];

static void app_uart_set_apply_error(const char *error_key)
{
    app_copy_string(s_uart.last_apply_error, sizeof(s_uart.last_apply_error),
                    error_key != NULL ? error_key : "uart_apply_failed");
}

static const char *app_uart_get_apply_error(void)
{
    return s_uart.last_apply_error[0] != '\0' ? s_uart.last_apply_error : "uart_apply_failed";
}

static void app_uart_build_preview_text(char *buffer, size_t buffer_size,
                                        const uint8_t *payload, size_t payload_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t index;
    size_t offset = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (payload == NULL || payload_len == 0) {
        return;
    }

    for (index = 0; index < payload_len && offset + 1 < buffer_size; index++) {
        unsigned char value = payload[index];

        if (isprint(value) && value != '\\') {
            buffer[offset++] = (char)value;
            continue;
        }
        if (offset + 4 >= buffer_size) {
            break;
        }
        buffer[offset++] = '\\';
        buffer[offset++] = 'x';
        buffer[offset++] = hex[(value >> 4) & 0x0F];
        buffer[offset++] = hex[value & 0x0F];
    }

    buffer[offset] = '\0';
}

static void app_uart_record_send(const char *encoding, const char *data,
                                 const uint8_t *payload, size_t payload_len,
                                 unsigned written)
{
    s_uart.tx_sequence += 1;
    s_uart.last_bytes_sent = written;
    s_uart.last_send_at_ms = esp_timer_get_time() / 1000;
    app_copy_string(s_uart.last_tx_encoding, sizeof(s_uart.last_tx_encoding),
                    encoding != NULL ? encoding : "plain");
    app_copy_string(s_uart.last_tx_data, sizeof(s_uart.last_tx_data),
                    data != NULL ? data : "");
    app_uart_build_preview_text(s_uart.last_tx_preview, sizeof(s_uart.last_tx_preview),
                                payload, payload_len);
    app_format_hex_bytes(payload, payload_len,
                         s_uart.last_tx_hex, sizeof(s_uart.last_tx_hex), ' ');
}

bool app_uart_lock(TickType_t wait_ticks)
{
    if (s_uart_op_mutex == NULL) {
        return true;
    }
    return xSemaphoreTakeRecursive(s_uart_op_mutex, wait_ticks) == pdTRUE;
}

void app_uart_unlock(void)
{
    if (s_uart_op_mutex != NULL) {
        xSemaphoreGiveRecursive(s_uart_op_mutex);
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

static bool app_uart_prepare_payload(const char *data, const char *encoding,
                                     uint8_t *tx_bytes, size_t tx_bytes_size,
                                     const uint8_t **payload, size_t *payload_len)
{
    if (data == NULL || payload == NULL || payload_len == NULL) {
        return false;
    }

    if (strcmp(encoding, "hex") == 0) {
        if (!app_uart_parse_hex_bytes(data, tx_bytes, tx_bytes_size, payload_len) || *payload_len == 0) {
            return false;
        }
        *payload = tx_bytes;
        return true;
    }

    *payload = (const uint8_t *)data;
    *payload_len = strlen(data);
    return *payload_len > 0;
}

static esp_err_t app_uart_send_payload(const char *encoding, const char *data,
                                       const uint8_t *payload, size_t payload_len,
                                       unsigned *bytes_sent)
{
    int written;

    if (!s_uart.driver_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }
    written = uart_write_bytes(UART_PORT, payload, payload_len);
    app_uart_unlock();
    if (written < 0) {
        return ESP_FAIL;
    }

    app_uart_record_send(encoding, data, payload, payload_len, (unsigned)written);
    if (bytes_sent != NULL) {
        *bytes_sent = (unsigned)written;
    }
    return ESP_OK;
}

static void app_uart_keepalive_task(void *arg)
{
    TickType_t next_wait = portMAX_DELAY;
    char encoding[8];
    char data[UART_TX_BUFFER_SIZE];
    uint8_t payload[UART_TX_BUFFER_SIZE];
    size_t payload_len = 0;
    uint32_t interval_ms = 1000;

    (void)arg;

    while (true) {
        if (ulTaskNotifyTake(pdTRUE, next_wait) > 0) {
            next_wait = 0;
            continue;
        }

        if (!app_uart_lock(pdMS_TO_TICKS(50))) {
            next_wait = pdMS_TO_TICKS(50);
            continue;
        }

        if (!s_uart.keepalive_active) {
            app_uart_unlock();
            next_wait = portMAX_DELAY;
            continue;
        }

        if (s_uart.keepalive_payload_len == 0) {
            app_uart_unlock();
            next_wait = pdMS_TO_TICKS(100);
            continue;
        }

        app_copy_string(encoding, sizeof(encoding), s_uart.keepalive_encoding);
        app_copy_string(data, sizeof(data), s_uart.keepalive_data);
        payload_len = s_uart.keepalive_payload_len;
        if (payload_len > sizeof(payload)) {
            payload_len = sizeof(payload);
        }
        memcpy(payload, s_uart.keepalive_payload, payload_len);
        interval_ms = s_uart.keepalive_interval_ms > 0 ? s_uart.keepalive_interval_ms : 1000;
        app_uart_unlock();

        {
            unsigned ignored = 0;
            esp_err_t err = app_uart_send_payload(encoding,
                                                  data,
                                                  payload,
                                                  payload_len,
                                                  &ignored);
            if (err != ESP_OK && err != ESP_ERR_INVALID_STATE && err != ESP_ERR_TIMEOUT) {
                ESP_LOGW(TAG, "keepalive send failed: %s", esp_err_to_name(err));
            }
        }

        next_wait = pdMS_TO_TICKS(interval_ms);
    }
}

static void app_uart_notify_keepalive_task(void)
{
    if (s_uart_keepalive_task != NULL) {
        xTaskNotifyGive(s_uart_keepalive_task);
    }
}

static esp_err_t app_uart_send_keepalive_json(httpd_req_t *req, const char *message)
{
    bool keepalive_active = false;
    uint32_t keepalive_interval_ms = 0;
    uint32_t tx_sequence = 0;
    int64_t keepalive_started_at_ms = 0;

    if (app_uart_lock(pdMS_TO_TICKS(100))) {
        keepalive_active = s_uart.keepalive_active;
        keepalive_interval_ms = s_uart.keepalive_interval_ms;
        tx_sequence = s_uart.tx_sequence;
        keepalive_started_at_ms = s_uart.keepalive_started_at_ms;
        app_uart_unlock();
    }

    return app_http_send_jsonf(req, NULL,
                           "{\"status\":\"ok\",\"message\":\"%s\","
                           "\"keepalive_active\":%s,\"keepalive_interval_ms\":%u,"
                           "\"keepalive_started_at_ms\":%lld,\"tx_sequence\":%u}",
                           message != NULL ? message : "",
                           keepalive_active ? "true" : "false",
                           (unsigned)keepalive_interval_ms,
                           (long long)keepalive_started_at_ms,
                           (unsigned)tx_sequence);
}

void app_uart_stop_keepalive(void)
{
    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        s_uart.keepalive_active = false;
        app_uart_notify_keepalive_task();
        return;
    }
    s_uart.keepalive_active = false;
    s_uart.keepalive_interval_ms = 0;
    s_uart.keepalive_started_at_ms = 0;
    s_uart.keepalive_payload_len = 0;
    s_uart.keepalive_encoding[0] = '\0';
    s_uart.keepalive_data[0] = '\0';
    memset(s_uart.keepalive_payload, 0, sizeof(s_uart.keepalive_payload));
    app_uart_unlock();
    app_uart_notify_keepalive_task();
}

void app_uart_init(void)
{
    s_uart_op_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_uart_op_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create uart op mutex");
    }
    s_json_mutex = xSemaphoreCreateMutex();
    if (s_json_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create json buffer mutex");
    }
    if (xTaskCreate(app_uart_keepalive_task, "uart_keepalive", 4096, NULL, 5, &s_uart_keepalive_task) != pdPASS) {
        ESP_LOGE(TAG, "Failed to create uart keepalive task");
        s_uart_keepalive_task = NULL;
    }
    app_uart_console_init();
}

void app_uart_build_frame(char *buffer, size_t buffer_size,
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

uart_parity_t app_uart_parity_from_string(const char *value)
{
    if (strcmp(value, "even") == 0 || strcmp(value, "8E1") == 0) {
        return UART_PARITY_EVEN;
    }
    if (strcmp(value, "odd") == 0 || strcmp(value, "8O1") == 0) {
        return UART_PARITY_ODD;
    }
    return UART_PARITY_DISABLE;
}

void app_uart_reset_runtime_state(void)
{
    app_uart_set_apply_error("");
    app_uart_console_reset();
}

esp_err_t app_uart_send_runtime_json(httpd_req_t *req, const char *message)
{
    char frame[8];
    int64_t last_send_age_ms = -1;
    uint32_t tx_sequence = 0;
    unsigned last_bytes_sent = 0;
    int64_t last_send_at_ms = 0;
    char last_tx_encoding[16];
    char keepalive_encoding[16];
    bool keepalive_active = false;
    uint32_t keepalive_interval_ms = 0;
    int64_t keepalive_started_at_ms = 0;
    bool driver_installed = false;
    esp_err_t result;
    bool json_locked = false;

    if (s_json_mutex != NULL) {
        if (xSemaphoreTake(s_json_mutex, pdMS_TO_TICKS(500)) != pdTRUE) {
            return app_http_send_json_text(req, "503 Service Unavailable",
                                       "{\"status\":\"error\",\"message\":\"json_buffer_busy\"}");
        }
        json_locked = true;
    }

    app_uart_build_frame(frame, sizeof(frame),
                         s_config.uart_data_bits,
                         (uart_parity_t)s_config.uart_parity_mode,
                         s_config.uart_stop_bits);

    s_escaped_tx_data[0] = '\0';
    s_escaped_tx_preview[0] = '\0';
    s_escaped_tx_hex[0] = '\0';
    s_escaped_keepalive_data[0] = '\0';
    last_tx_encoding[0] = '\0';
    keepalive_encoding[0] = '\0';

    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        if (json_locked && s_json_mutex != NULL) {
            xSemaphoreGive(s_json_mutex);
        }
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_busy\"}");
    }
    app_json_escape_string(s_escaped_tx_data, sizeof(s_escaped_tx_data), s_uart.last_tx_data);
    app_json_escape_string(s_escaped_tx_preview, sizeof(s_escaped_tx_preview), s_uart.last_tx_preview);
    app_json_escape_string(s_escaped_tx_hex, sizeof(s_escaped_tx_hex), s_uart.last_tx_hex);
    app_json_escape_string(s_escaped_keepalive_data, sizeof(s_escaped_keepalive_data), s_uart.keepalive_data);
    if (s_uart.last_send_at_ms > 0) {
        int64_t now_ms = esp_timer_get_time() / 1000;
        last_send_age_ms = now_ms > s_uart.last_send_at_ms ? (now_ms - s_uart.last_send_at_ms) : 0;
    }
    tx_sequence = s_uart.tx_sequence;
    last_bytes_sent = s_uart.last_bytes_sent;
    last_send_at_ms = s_uart.last_send_at_ms;
    driver_installed = s_uart.driver_installed;
    keepalive_active = s_uart.keepalive_active;
    keepalive_interval_ms = s_uart.keepalive_interval_ms;
    keepalive_started_at_ms = s_uart.keepalive_started_at_ms;
    app_copy_string(last_tx_encoding, sizeof(last_tx_encoding),
                    s_uart.last_tx_encoding[0] != '\0' ? s_uart.last_tx_encoding : "");
    app_copy_string(keepalive_encoding, sizeof(keepalive_encoding),
                    s_uart.keepalive_encoding[0] != '\0' ? s_uart.keepalive_encoding : "");
    app_uart_unlock();

    result = app_http_send_jsonf(req, NULL,
                           "{\"status\":\"ok\",\"message\":\"%s\","
                           "\"rx_gpio\":%ld,\"tx_gpio\":%ld,\"baudrate\":%u,"
                           "\"data_bits\":%u,\"stop_bits\":%u,\"parity\":\"%s\","
                            "\"frame\":\"%s\",\"driver_installed\":%s,"
                            "\"tx_sequence\":%u,\"last_bytes_sent\":%u,"
                            "\"last_send_at_ms\":%lld,\"last_send_age_ms\":%lld,"
                            "\"last_payload_encoding\":\"%s\",\"last_payload_data\":\"%s\","
                              "\"last_payload_preview\":\"%s\",\"last_payload_hex\":\"%s\","
                              "\"keepalive_active\":%s,\"keepalive_interval_ms\":%u,"
                              "\"keepalive_started_at_ms\":%lld,"
                              "\"keepalive_encoding\":\"%s\",\"keepalive_data\":\"%s\"}",
                           message != NULL ? message : "",
                           (long)app_uart_rx_gpio(),
                           (long)app_uart_tx_gpio(),
                           s_config.uart_baudrate,
                           (unsigned)s_config.uart_data_bits,
                           (unsigned)s_config.uart_stop_bits,
                           app_uart_parity_to_string((uart_parity_t)s_config.uart_parity_mode),
                           frame,
                            driver_installed ? "true" : "false",
                            (unsigned)tx_sequence,
                            last_bytes_sent,
                            (long long)last_send_at_ms,
                            (long long)last_send_age_ms,
                            last_tx_encoding,
                            s_escaped_tx_data,
                            s_escaped_tx_preview,
                              s_escaped_tx_hex,
                              keepalive_active ? "true" : "false",
                              (unsigned)keepalive_interval_ms,
                              (long long)keepalive_started_at_ms,
                              keepalive_encoding,
                              s_escaped_keepalive_data);

    if (json_locked && s_json_mutex != NULL) {
        xSemaphoreGive(s_json_mutex);
    }
    return result;
}

static esp_err_t app_uart_apply_config_values(uint32_t baudrate, uint8_t data_bits,
                                              uart_parity_t parity, uint8_t stop_bits)
{
    esp_err_t err;
    uart_sclk_t source_clk;
    uart_config_t uart_config;
    int retry;

    source_clk = (baudrate < 4800) ? UART_SCLK_REF_TICK : UART_SCLK_DEFAULT;
    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baud_rate = (int)baudrate;
    uart_config.data_bits = app_uart_data_bits_to_enum(data_bits);
    uart_config.parity = parity;
    uart_config.stop_bits = app_uart_stop_bits_to_enum(stop_bits);
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = source_clk;

    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        app_uart_set_apply_error("uart_busy");
        return ESP_ERR_TIMEOUT;
    }

    uart_driver_delete(UART_PORT);
    s_uart.driver_installed = false;
    vTaskDelay(pdMS_TO_TICKS(20));

    app_uart_set_apply_error("uart_apply_failed");

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

        gpio_reset_pin(app_uart_rx_gpio());
        gpio_reset_pin(app_uart_tx_gpio());
        gpio_pullup_en(app_uart_rx_gpio());
        gpio_pulldown_dis(app_uart_rx_gpio());

        err = uart_set_pin(UART_PORT, app_uart_tx_gpio(), app_uart_rx_gpio(),
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_set_pin_failed");
            ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
            continue;
        }
        /* uart_set_pin (via gpio_matrix_input) only enables input but does not
         * set pull-up. Re-enable pull-up after pin assignment so the RX line
         * is not left floating. */
        gpio_pullup_en(app_uart_rx_gpio());
        gpio_pulldown_dis(app_uart_rx_gpio());

        err = uart_driver_install(UART_PORT, UART_RX_BUFFER_SIZE * 2, 0, 0, NULL, 0);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_driver_install_failed");
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
            continue;
        }

        s_uart.driver_installed = true;
        app_uart_set_apply_error("");
        app_uart_console_reset();
        uart_flush_input(UART_PORT);
        app_uart_unlock();
        return ESP_OK;
    }

    app_uart_unlock();
    return err;
}

esp_err_t app_uart_apply_config(void)
{
    return app_uart_apply_config_values(s_config.uart_baudrate,
                                        s_config.uart_data_bits,
                                        (uart_parity_t)s_config.uart_parity_mode,
                                        s_config.uart_stop_bits);
}

esp_err_t app_uart_handle_config_request(httpd_req_t *req, const char *body)
{
    const uint32_t old_baudrate = s_config.uart_baudrate;
    const uint8_t old_parity_mode = s_config.uart_parity_mode;
    const uint8_t old_data_bits = s_config.uart_data_bits;
    const uint8_t old_stop_bits = s_config.uart_stop_bits;
    uint32_t new_baudrate = old_baudrate;
    uint8_t new_parity_mode = old_parity_mode;
    uint8_t new_data_bits = old_data_bits;
    uint8_t new_stop_bits = old_stop_bits;

    char apply_error[48];
    char value[64];
    uint32_t baudrate;
    uint16_t short_value;

    if (app_json_find_u32(body, "baudrate", &baudrate) && baudrate > 0) {
        new_baudrate = baudrate;
    }
    if (app_json_find_string(body, "parity", value, sizeof(value))) {
        new_parity_mode = (uint8_t)app_uart_parity_from_string(value);
    }
    if (app_json_find_u16(body, "data_bits", &short_value)) {
        if (!app_uart_data_bits_valid((uint8_t)short_value)) {
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"invalid_uart_data_bits\"}");
        }
        new_data_bits = (uint8_t)short_value;
    }
    if (app_json_find_u16(body, "stop_bits", &short_value)) {
        if (!app_uart_stop_bits_valid((uint8_t)short_value)) {
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"invalid_uart_stop_bits\"}");
        }
        new_stop_bits = (uint8_t)short_value;
    }

    if (app_uart_apply_config_values(new_baudrate, new_data_bits,
                                     (uart_parity_t)new_parity_mode,
                                     new_stop_bits) != ESP_OK) {
        app_copy_string(apply_error, sizeof(apply_error), app_uart_get_apply_error());
        if (app_uart_apply_config_values(old_baudrate, old_data_bits,
                                         (uart_parity_t)old_parity_mode,
                                         old_stop_bits) != ESP_OK) {
            return app_http_send_json_text(req, "500 Internal Server Error",
                                       "{\"status\":\"error\",\"message\":\"uart_restore_failed\"}");
        }
        app_uart_set_apply_error(apply_error);
        return app_http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"%s\"}",
                               apply_error);
    }
    app_config_t snapshot = s_config;
    snapshot.uart_baudrate = new_baudrate;
    snapshot.uart_parity_mode = new_parity_mode;
    snapshot.uart_data_bits = new_data_bits;
    snapshot.uart_stop_bits = new_stop_bits;
    if (app_config_save(&snapshot) != ESP_OK) {
        if (app_uart_apply_config_values(old_baudrate, old_data_bits,
                                         (uart_parity_t)old_parity_mode,
                                         old_stop_bits) != ESP_OK) {
            return app_http_send_json_text(req, "500 Internal Server Error",
                                       "{\"status\":\"error\",\"message\":\"uart_restore_failed\"}");
        }
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"config_save_failed\"}");
    }

    s_config.uart_baudrate = new_baudrate;
    s_config.uart_parity_mode = new_parity_mode;
    s_config.uart_data_bits = new_data_bits;
    s_config.uart_stop_bits = new_stop_bits;
    return app_uart_send_runtime_json(req, "uart_runtime");
}

esp_err_t app_uart_handle_send_request(httpd_req_t *req, const char *body)
{
    char data[UART_TX_BUFFER_SIZE];
    char encoding[8] = "plain";
    unsigned written = 0;
    esp_err_t err;

    if (!s_uart.driver_installed) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_not_ready\"}");
    }
    if (!app_json_find_string(body, "data", data, sizeof(data))) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"uart_data_required\"}");
    }
    app_json_find_string(body, "encoding", encoding, sizeof(encoding));
    err = app_uart_send_data(encoding, data, &written);
    if (err == ESP_ERR_TIMEOUT) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_busy\"}");
    }
    if (err == ESP_ERR_INVALID_STATE) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_not_ready\"}");
    }
    if (err != ESP_OK) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"uart_send_failed\"}");
    }

    return app_http_send_jsonf(req, NULL,
                           "{\"status\":\"ok\",\"bytes_sent\":%u}",
                           written);
}

esp_err_t app_uart_send_data(const char *encoding, const char *data, unsigned *bytes_sent)
{
    uint8_t tx_bytes[UART_TX_BUFFER_SIZE / 2];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (!app_uart_prepare_payload(data, encoding != NULL ? encoding : "plain",
                                  tx_bytes, sizeof(tx_bytes), &payload, &payload_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    return app_uart_send_payload(encoding != NULL ? encoding : "plain",
                                 data != NULL ? data : "",
                                 payload, payload_len, bytes_sent);
}

esp_err_t app_uart_handle_keepalive_start_request(httpd_req_t *req, const char *body)
{
    char data[UART_TX_BUFFER_SIZE];
    char encoding[8] = "plain";
    uint8_t tx_bytes[UART_TX_BUFFER_SIZE / 2];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;
    uint32_t interval_ms = 1000;

    if (!s_uart.driver_installed) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_not_ready\"}");
    }
    if (!app_json_find_string(body, "data", data, sizeof(data))) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"uart_data_required\"}");
    }
    app_json_find_string(body, "encoding", encoding, sizeof(encoding));
    if (app_json_find_u32(body, "interval_ms", &interval_ms)) {
        if (!(interval_ms == 200 || interval_ms == 500 || interval_ms == 1000)) {
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"invalid_interval_ms\"}");
        }
    }

    if (!app_uart_prepare_payload(data, encoding, tx_bytes, sizeof(tx_bytes), &payload, &payload_len)) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   strcmp(encoding, "hex") == 0
                                       ? "{\"status\":\"error\",\"message\":\"uart_hex_invalid\"}"
                                       : "{\"status\":\"error\",\"message\":\"uart_data_required\"}");
    }

    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_busy\"}");
    }
    app_copy_string(s_uart.keepalive_encoding, sizeof(s_uart.keepalive_encoding), encoding);
    app_copy_string(s_uart.keepalive_data, sizeof(s_uart.keepalive_data), data);
    memcpy(s_uart.keepalive_payload, payload, payload_len);
    s_uart.keepalive_payload_len = payload_len;
    s_uart.keepalive_interval_ms = interval_ms;
    s_uart.keepalive_started_at_ms = esp_timer_get_time() / 1000;
    s_uart.keepalive_active = true;
    app_uart_unlock();
    app_uart_notify_keepalive_task();

    return app_uart_send_keepalive_json(req, "uart_keepalive_started");
}

esp_err_t app_uart_handle_keepalive_stop_request(httpd_req_t *req)
{
    app_uart_stop_keepalive();
    return app_uart_send_keepalive_json(req, "uart_keepalive_stopped");
}
