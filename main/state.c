#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_log.h"

#include "config.h"
#include "constants.h"
#include "state.h"

static const char *TAG = "state";

extern app_config_t s_config;

gpio_num_t app_relay_gpio(void)
{
    return RELAY_DEFAULT_GPIO;
}

gpio_num_t app_led_gpio(void)
{
    return LED_DEFAULT_GPIO;
}

gpio_num_t app_input_gpio(void)
{
    return INPUT_DEFAULT_GPIO;
}

gpio_num_t app_uart_rx_gpio(void)
{
    return UART_RX_GPIO;
}

gpio_num_t app_uart_tx_gpio(void)
{
    return UART_TX_GPIO;
}

void app_apply_output(gpio_num_t gpio_num, bool active_high, bool on)
{
    gpio_set_level(gpio_num, (active_high ? on : !on) ? 1 : 0);
}

bool app_get_input_state(void)
{
    int raw = gpio_get_level(app_input_gpio());
    return s_config.input_active_low ? (raw == 0) : (raw != 0);
}

void app_configure_gpio(void)
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
    const gpio_config_t boot_button_cfg = {
        .pin_bit_mask = (1ULL << APP_FACTORY_RESET_BOOT_GPIO),
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
        .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_DISABLE,
    };

    ESP_ERROR_CHECK(gpio_config(&output_cfg));
    ESP_ERROR_CHECK(gpio_config(&input_cfg));
    ESP_ERROR_CHECK(gpio_config(&boot_button_cfg));

    app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
    app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
}
