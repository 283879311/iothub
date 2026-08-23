#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"
#include "esp_http_server.h"
#include "freertos/FreeRTOS.h"

void app_uart_init(void);
void app_uart_reset_runtime_state(void);
void app_uart_build_frame(char *buffer, size_t buffer_size,
                          uint8_t data_bits, uart_parity_t parity, uint8_t stop_bits);
uart_parity_t app_uart_parity_from_string(const char *value);
esp_err_t app_uart_send_runtime_json(httpd_req_t *req, const char *message);
esp_err_t app_uart_apply_config(void);
esp_err_t app_uart_handle_config_request(httpd_req_t *req, const char *body);
esp_err_t app_uart_send_console_json(httpd_req_t *req);
esp_err_t app_uart_handle_clear_console_request(httpd_req_t *req);
esp_err_t app_uart_handle_send_request(httpd_req_t *req, const char *body);
esp_err_t app_uart_send_data(const char *encoding, const char *data, unsigned *bytes_sent);
esp_err_t app_uart_handle_keepalive_start_request(httpd_req_t *req, const char *body);
esp_err_t app_uart_handle_keepalive_stop_request(httpd_req_t *req);
void app_uart_stop_keepalive(void);
