#pragma once

#include "sdkconfig.h"

#include "driver/gpio.h"
#include "driver/uart.h"

#define APP_JSON_BUFFER_SIZE 4096
#define APP_OTA_REBOOT_DELAY_MS 1500
#define APP_FACTORY_RESET_BOOT_GPIO GPIO_NUM_0
#define APP_FACTORY_RESET_HOLD_MS 5000
#define APP_FACTORY_RESET_POLL_MS 100

/* 引脚唯一来源:Kconfig -> sdkconfig -> CONFIG_*;改脚只需 menuconfig/改 sdkconfig。 */
#define RELAY_DEFAULT_GPIO ((gpio_num_t)CONFIG_RELAY_GPIO)
#define LED_DEFAULT_GPIO   ((gpio_num_t)CONFIG_LED_GPIO)
#define INPUT_DEFAULT_GPIO ((gpio_num_t)CONFIG_INPUT_GPIO)
#define UART_PORT UART_NUM_1
#define UART_RX_GPIO GPIO_NUM_18
#define UART_TX_GPIO GPIO_NUM_17
#define UART_RX_BUFFER_SIZE 2048
#define UART_CONSOLE_PLAIN_SIZE 1024
#define UART_CONSOLE_HEX_SIZE 3072
#define UART_TX_BUFFER_SIZE 256
#define UART_TX_PREVIEW_SIZE 192
#define UART_TX_HEX_TEXT_SIZE 256

#define WIFI_SCAN_LIST_SIZE 8
#define APP_WIFI_PROFILE_MAX 5

/* 蓝牙模式:v1.2 起移除 SPP(经典蓝牙),历史配置中的旧值 2 由
 * app_bt_apply_config() 归一为 BT_MODE_OFF */
#define BT_MODE_OFF 0
#define BT_MODE_BLE 1

/* 蓝牙角色(v8 起,权威字段):
 *   0=off        不启用蓝牙
 *   1=peripheral 从角色:广播 + NUS 数据服务(手机/PC 连本机)
 *   2=central    主角色:主动扫描并连接对端(如另一台 NUS 从机)
 * 角色不可热切(官方:初始化后不能直接改角色,必须 deinit 或重启)→ 改角色即重启 */
#define BT_ROLE_OFF 0
#define BT_ROLE_PERIPHERAL 1
#define BT_ROLE_CENTRAL 2

#define NET_MODE_AP 0
#define NET_MODE_STA 1
#define NET_MODE_APSTA 2
