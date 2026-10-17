#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================================
 * BLE 数据面胶合层:把 GATT 服务接到 data_pipe
 *   - 出站:B 端口 write 函数 = app_ble_gatts_notify()
 *   - 入站:GATT WRITE 回调 → app_ble_dataplane_on_write() → data_pipe_feed()
 * ========================================================================== */

/* 启动 BLE 数据面:注册 BLE 端口 + 注册数据面 GATT 服务 */
esp_err_t app_ble_dataplane_start(void);

/* 启动主角色数据面:BLE 端口 = 写对端 RX 特征 + 启动 GATT Client(扫描/连接) */
esp_err_t app_ble_dataplane_start_central(void);

/* 拆栈时清理(供 bt.c 调用) */
void app_ble_dataplane_reset(void);

/* 供 bt_gatts 的回调调用(只入队,不阻塞) */
void app_ble_dataplane_on_write(const uint8_t *data, size_t len);

bool app_ble_dataplane_ready(void);
