#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "driver/uart.h"
#include "esp_err.h"

#include "constants.h"
#include "freertos/FreeRTOS.h"

void app_uart_init(void);
void app_uart_reset_runtime_state(void);
void app_uart_build_frame(char *buffer, size_t buffer_size,
                          uint8_t data_bits, uart_parity_t parity, uint8_t stop_bits);
uart_parity_t app_uart_parity_from_string(const char *value);
esp_err_t app_uart_apply_config(void);
esp_err_t app_uart_send_data(const char *encoding, const char *data, unsigned *bytes_sent);

/* 数据面:按长度发送原始字节(二进制安全)+ data_pipe 端口出站函数 */
esp_err_t app_uart_send_bytes(const uint8_t *data, size_t len, unsigned *bytes_sent);
bool app_uart_dataplane_write(const uint8_t *data, size_t len, void *ctx);

/* UART 运行态快照(只读;uart.runtime 命令用):
 * 驱动状态、收发序号、最近发送摘要/错误。与 apply 重配置互斥。 */
typedef struct {
    bool driver_installed;
    uint32_t rx_sequence;
    uint32_t tx_sequence;
    uint32_t last_bytes_sent;
    int64_t last_send_at_ms;
    char last_apply_error[48];
    char last_tx_encoding[8];
    char last_tx_preview[UART_TX_PREVIEW_SIZE];
} app_uart_status_t;

bool app_uart_get_status(app_uart_status_t *out);
