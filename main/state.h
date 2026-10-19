#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "driver/gpio.h"
#include "esp_err.h"

gpio_num_t app_relay_gpio(void);
gpio_num_t app_led_gpio(void);
gpio_num_t app_input_gpio(void);
gpio_num_t app_uart_rx_gpio(void);
gpio_num_t app_uart_tx_gpio(void);

/* 输出状态变化来源(用于日志/状态上报;不改变控制语义)。 */
typedef enum {
    APP_IO_SOURCE_BOOT = 0,
    APP_IO_SOURCE_COMMAND,
    APP_IO_SOURCE_HTTP,
    APP_IO_SOURCE_INPUT,
    APP_IO_SOURCE_MQTT,
    APP_IO_SOURCE_BLE,
} app_io_source_t;

/* 通用“连续 N 次一致才稳定”的去抖状态;触摸和数字输入共用。
 * 每个实例只由单一采样上下文调用,不做内部加锁。 */
typedef struct {
    bool stable;
    bool candidate;
    uint8_t count;
} app_debounce_t;

/* 输入 sample;达到 required 次一致且状态变化时返回 true,并更新 stable。 */
bool app_debounce_update(app_debounce_t *db, bool sample, uint8_t required);

typedef struct {
    bool relay_on;          /* 实际输出状态 */
    bool led_on;
    bool input_active;      /* 读取时实时采样 */
    uint32_t seq;           /* 输出状态每次变化 +1 */
    int64_t last_change_ms; /* 设备运行毫秒(非 epoch);无变化为 0 */
    app_io_source_t last_source;
} app_io_state_t;

void app_io_get_state(app_io_state_t *out);
const char *app_io_source_name(app_io_source_t source);

/* 设置输出:set_relay/set_led 为 false 表示保持该路不变。
 * 只对实际变化且策略为“状态记忆”的输出写 NVS;固定策略绝不写。 */
esp_err_t app_io_set_outputs(bool set_relay, bool relay_on,
                             bool set_led, bool led_on,
                             app_io_source_t source);

/* 配置域变化后调用:固定策略立即生效,状态记忆策略保存当前状态供下次上电恢复。 */
void app_io_apply_policies_at_runtime(void);

esp_err_t app_apply_output(gpio_num_t gpio_num, bool active_high, bool on);
bool app_get_input_state(void);
void app_configure_gpio(void);
