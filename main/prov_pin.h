#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* BLE 配网 PoP（官方 App 里显示的 PIN）。
 * 默认值与 ESP BLE Provisioning App 示例一致；只存 NVS，不进入 config blob，
 * 不在 config.get/日志/命令响应中回显。 */
#define APP_PROV_PIN_DEFAULT "abcd1234"
#define APP_PROV_PIN_MAX_LEN 64

bool app_prov_pin_is_valid(const char *pin);
esp_err_t app_prov_pin_get(char *out, size_t out_len);
esp_err_t app_prov_pin_set(const char *pin);
/* 恢复出厂：清除 PIN 以及探针的 ble_prov/done 标记。 */
esp_err_t app_prov_state_clear(void);
