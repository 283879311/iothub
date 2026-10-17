#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================================
 * BLE 命令外壳(1.3-32):CMD 特征(独立 UUID)行重组 → 分发(cmd.c 内核)→ 回包。
 * 数据特征(RX/TX)保持纯透传,命令与数据在 GATT 层物理隔离,零歧义。
 * ========================================================================== */

esp_err_t app_ble_cmd_start(void);                    /* 幂等,启动分发任务 */
void app_ble_cmd_on_write(const uint8_t *data, size_t len);  /* CMD RX 写回调(任意上下文,只入队) */
void app_ble_cmd_on_disconnect(void);                 /* 断连:作废短时 token */
