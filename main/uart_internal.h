#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "constants.h"
#include "freertos/FreeRTOS.h"

typedef struct {
    bool driver_installed;
    bool keepalive_active;
    uint32_t rx_sequence;
    uint32_t tx_sequence;
    uint32_t last_bytes_sent;
    uint32_t keepalive_interval_ms;
    int64_t last_send_at_ms;
    int64_t keepalive_started_at_ms;
    char last_apply_error[48];
    char last_tx_encoding[8];
    char last_tx_data[UART_TX_BUFFER_SIZE];
    char last_tx_preview[UART_TX_PREVIEW_SIZE];
    char last_tx_hex[UART_TX_HEX_TEXT_SIZE];
    char keepalive_encoding[8];
    char keepalive_data[UART_TX_BUFFER_SIZE];
    uint8_t keepalive_payload[UART_TX_BUFFER_SIZE];
    size_t keepalive_payload_len;
    char rx_plain[UART_CONSOLE_PLAIN_SIZE];
    char rx_hex[UART_CONSOLE_HEX_SIZE];
} uart_runtime_t;

extern uart_runtime_t s_uart;

bool app_uart_lock(TickType_t wait_ticks);
void app_uart_unlock(void);
