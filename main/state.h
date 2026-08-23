#pragma once

#include <stdbool.h>

#include "driver/gpio.h"

gpio_num_t app_relay_gpio(void);
gpio_num_t app_led_gpio(void);
gpio_num_t app_input_gpio(void);
gpio_num_t app_uart_rx_gpio(void);
gpio_num_t app_uart_tx_gpio(void);

void app_apply_output(gpio_num_t gpio_num, bool active_high, bool on);
bool app_get_input_state(void);
void app_configure_gpio(void);
