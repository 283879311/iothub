#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "common.h"
#include "constants.h"
#include "http_utils.h"
#include "state.h"
#include "uart.h"
#include "uart_console.h"
#include "uart_internal.h"

static const char *TAG = "uart";

static SemaphoreHandle_t s_uart_console_mutex = NULL;
static TaskHandle_t s_uart_console_task = NULL;
static size_t s_uart_last_available = 0;
static int s_uart_last_read_len = -1;
static uint32_t s_uart_poll_count = 0;
static int s_uart_last_rx_level = -1;
static int s_uart_last_tx_level = -1;

static void app_uart_poll_console(TickType_t wait_ticks);

static void app_uart_console_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        app_uart_poll_console(0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
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
    char plain_chunk[UART_SAMPLE_BUFFER_SIZE * 4 + 2];
    char hex_chunk[UART_SAMPLE_BUFFER_SIZE * 3 + 2];
    size_t plain_index = 0;
    size_t hex_index = 0;
    size_t index;

    for (index = 0; index < raw_len; index++) {
        uint8_t ch = raw[index];
        const char *escape = NULL;
        int written;

        if (ch == '\r' || ch == '\n' || ch == '\t' || (ch >= 32 && ch <= 126)) {
            if (plain_index + 1 < sizeof(plain_chunk)) {
                plain_chunk[plain_index++] = (char)ch;
            }
        } else {
            switch (ch) {
            case 0x00:
                escape = "\\0";
                break;
            case 0x02:
                escape = "\\x02";
                break;
            case 0x03:
                escape = "\\x03";
                break;
            default:
                break;
            }
            if (escape != NULL) {
                written = snprintf(plain_chunk + plain_index,
                                   sizeof(plain_chunk) - plain_index,
                                   "%s",
                                   escape);
            } else {
                written = snprintf(plain_chunk + plain_index,
                                   sizeof(plain_chunk) - plain_index,
                                   "\\x%02X",
                                   ch);
            }
            if (written > 0) {
                size_t append_len = (size_t)written;
                if (append_len >= sizeof(plain_chunk) - plain_index) {
                    plain_index = sizeof(plain_chunk) - 1;
                } else {
                    plain_index += append_len;
                }
            }
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

    app_append_tail(s_uart.rx_plain, sizeof(s_uart.rx_plain), plain_chunk);
    app_append_tail(s_uart.rx_hex, sizeof(s_uart.rx_hex), hex_chunk);
    s_uart.rx_sequence++;
}

static int app_uart_read_raw(uint8_t *raw, size_t raw_size, TickType_t wait_ticks)
{
    size_t available = 0;

    if (!s_uart.driver_installed || raw_size == 0) {
        s_uart_last_available = 0;
        s_uart_last_read_len = -1;
        s_uart_last_rx_level = gpio_get_level(app_uart_rx_gpio());
        s_uart_last_tx_level = gpio_get_level(app_uart_tx_gpio());
        return -1;
    }

    s_uart_last_rx_level = gpio_get_level(app_uart_rx_gpio());
    s_uart_last_tx_level = gpio_get_level(app_uart_tx_gpio());
    uart_get_buffered_data_len(UART_PORT, &available);
    if (available == 0 && wait_ticks > 0) {
        vTaskDelay(wait_ticks);
        s_uart_last_rx_level = gpio_get_level(app_uart_rx_gpio());
        s_uart_last_tx_level = gpio_get_level(app_uart_tx_gpio());
        uart_get_buffered_data_len(UART_PORT, &available);
    }

    if (available == 0) {
        s_uart_last_available = 0;
        s_uart_last_read_len = 0;
        return 0;
    }

    if (available > raw_size) {
        available = raw_size;
    }

    s_uart_last_available = available;
    s_uart_last_read_len = uart_read_bytes(UART_PORT, raw, available, pdMS_TO_TICKS(50));
    ESP_LOGD(TAG, "uart_read: available=%u read=%d", (unsigned)available, s_uart_last_read_len);
    return s_uart_last_read_len;
}

static esp_err_t app_uart_send_console_json_body(httpd_req_t *req,
                                             const char *plain_json,
                                             const char *hex_json,
                                             unsigned rx_sequence)
{
    char *response = NULL;
    int resp_len;
    size_t response_size;
    esp_err_t err;

    response_size = UART_CONSOLE_PLAIN_SIZE * 2 + UART_CONSOLE_HEX_SIZE * 2 + 256;
    response = malloc(response_size);
    if (response == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    resp_len = snprintf(response, response_size,
                        "{\"plain\":\"%s\",\"hex\":\"%s\","
                        "\"rx_sequence\":%u,\"driver_installed\":%s,"
                        "\"debug_available\":%u,\"debug_read_len\":%d,\"debug_poll_count\":%u,"
                        "\"debug_rx_level\":%d,\"debug_tx_level\":%d}",
                        plain_json, hex_json, rx_sequence,
                        s_uart.driver_installed ? "true" : "false",
                        (unsigned)s_uart_last_available,
                        s_uart_last_read_len,
                        (unsigned)s_uart_poll_count,
                        s_uart_last_rx_level,
                        s_uart_last_tx_level);
    if (resp_len < 0 || (size_t)resp_len >= response_size) {
        free(response);
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"response_too_large\"}");
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    httpd_resp_set_hdr(req, "Access-Control-Allow-Origin", "*");
    err = httpd_resp_sendstr(req, response);
    free(response);
    return err;
}

void app_uart_console_init(void)
{
    s_uart_console_mutex = xSemaphoreCreateRecursiveMutex();
    if (s_uart_console_mutex == NULL) {
        ESP_LOGE(TAG, "Failed to create uart console mutex");
    }
    if (s_uart_console_task == NULL &&
        xTaskCreate(app_uart_console_task_fn, "uart_console", 3072, NULL, 5, &s_uart_console_task) != pdPASS) {
        s_uart_console_task = NULL;
        ESP_LOGE(TAG, "Failed to create uart console task");
    }
}

void app_uart_console_reset(void)
{
    s_uart.rx_sequence = 0;
    app_copy_string(s_uart.rx_plain, sizeof(s_uart.rx_plain), "");
    app_copy_string(s_uart.rx_hex, sizeof(s_uart.rx_hex), "");
}

static void app_uart_poll_console(TickType_t wait_ticks)
{
    uint8_t raw[UART_SAMPLE_BUFFER_SIZE];
    int read_len;

    s_uart_poll_count++;
    if (!app_uart_lock(pdMS_TO_TICKS(100))) {
        return;
    }
    if (!s_uart.driver_installed) {
        app_uart_unlock();
        return;
    }
    if (s_uart_console_mutex == NULL) {
        app_uart_unlock();
        return;
    }
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        app_uart_unlock();
        return;
    }

    read_len = app_uart_read_raw(raw, sizeof(raw), wait_ticks);
    if (read_len > 0) {
        app_uart_append_console(raw, (size_t)read_len);
    }

    xSemaphoreGiveRecursive(s_uart_console_mutex);
    app_uart_unlock();
}

esp_err_t app_uart_send_console_json(httpd_req_t *req)
{
    char *plain_json = NULL;
    char *hex_json = NULL;
    esp_err_t err;
    bool uart_locked = false;
    bool console_locked = false;
    unsigned rx_sequence;

    plain_json = malloc(UART_CONSOLE_PLAIN_SIZE * 2 + 1);
    hex_json = malloc(UART_CONSOLE_HEX_SIZE * 2 + 1);
    if (plain_json == NULL || hex_json == NULL) {
        free(plain_json);
        free(hex_json);
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"out_of_memory\"}");
    }

    if (s_uart_console_mutex == NULL) {
        free(plain_json);
        free(hex_json);
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_mutex_not_ready\"}");
    }
    if (!app_uart_lock(pdMS_TO_TICKS(300))) {
        free(plain_json);
        free(hex_json);
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_port_busy\"}");
    }
    uart_locked = true;
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
        app_uart_unlock();
        free(plain_json);
        free(hex_json);
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_console_busy\"}");
    }
    console_locked = true;
    app_uart_poll_console(0);
    app_json_escape_string(plain_json, UART_CONSOLE_PLAIN_SIZE * 2 + 1, s_uart.rx_plain);
    app_json_escape_string(hex_json, UART_CONSOLE_HEX_SIZE * 2 + 1, s_uart.rx_hex);
    rx_sequence = (unsigned)s_uart.rx_sequence;

    err = app_uart_send_console_json_body(req, plain_json, hex_json, rx_sequence);
    if (console_locked) {
        xSemaphoreGiveRecursive(s_uart_console_mutex);
    }
    if (uart_locked) {
        app_uart_unlock();
    }
    free(plain_json);
    free(hex_json);
    return err;
}

esp_err_t app_uart_handle_clear_console_request(httpd_req_t *req)
{
    bool uart_locked = false;
    bool console_locked = false;

    if (s_uart_console_mutex == NULL) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_mutex_not_ready\"}");
    }
    if (!app_uart_lock(pdMS_TO_TICKS(300))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_port_busy\"}");
    }
    uart_locked = true;
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
        app_uart_unlock();
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_console_busy\"}");
    }
    console_locked = true;
    app_uart_console_reset();
    if (console_locked) {
        xSemaphoreGiveRecursive(s_uart_console_mutex);
    }
    if (uart_locked) {
        app_uart_unlock();
    }

    return app_http_send_json_text(req, NULL,
                               "{\"status\":\"ok\",\"message\":\"uart_console_cleared\"}");
}
