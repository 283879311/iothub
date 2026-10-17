#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================================
 * BLE 数据面 GATT 客户端(central / 主角色)
 *
 * 用途:设备主动扫描并连接对端 NUS 从机(如另一台 iothub 或任意 NUS 外设),
 *       把对端通知的数据投入 data_pipe,把本地数据写回对端 RX 特征。
 * 与 peripheral 的分工:主角色下 bt.c 不注册 GAP 广播回调,由本模块独占扫描,
 *       避免两处争用同一个 GAP 回调(BLE 角色不可热切:切换靠重启)。
 * ========================================================================== */

/* 启动主角色:注册 GATTC 应用 + GAP 扫描回调 + 开始扫描 NUS 从机 */
esp_err_t app_ble_gattc_start(void);

/* 拆栈时清理 */
void app_ble_gattc_reset(void);

/* 数据面出站:写入对端 RX 特征(按 MTU 分片);返回 false = 未送出 */
bool app_ble_gattc_write(const uint8_t *data, size_t len, void *ctx);

bool app_ble_gattc_connected(void);

#if CONFIG_BT_NIMBLE_ENABLED
/* NimBLE:host sync 后由 bt.c 转发调用,开始扫描(扫描不能在 sync 前启动) */
void app_ble_gattc_nimble_on_sync(void);
#endif
