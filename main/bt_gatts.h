#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================================
 * BLE 数据面 GATT 服务(peripheral 侧)
 *
 * 服务形态:复用行业事实标准 Nordic UART Service(UUID 6E400001-...),
 *   手机端 nRF Connect / 通用 BLE 串口 App 免开发即可联调。
 *   - RX 特征(6E400002):Write / WriteWithoutResponse ← 中心设备写入(纯数据)
 *   - TX 特征(6E400003):Notify → 设备上行(纯数据)
 *   - CMD RX(6E400004):Write ← 命令帧(JSON 行,行重组后走命令分发器,1.3-32)
 *   - CMD TX(6E400005):Notify → 命令回包(与数据特征物理隔离)
 *
 * 注:BLE 配网(protocomm)已按 问题与版本优化.md 第 44 项退役(改走 SoftAP 门户),
 *   本数据服务是 BLE 侧唯一注册的 GATT 服务。
 *
 * 硬约束(GATT API 官方说明):回调内不得做耗时操作 → 本模块回调只做
 *   memcpy + 入队(data_pipe_feed / ble_cmd 行重组),实际发送在任务里完成。
 * ========================================================================== */

/* 启动数据面 GATT(需蓝牙栈已就绪;由 transport_ble 调用) */
esp_err_t app_ble_gatts_start(void);

/* 清空连接/MTU/拥塞状态(拆栈时调用) */
void app_ble_gatts_reset(void);

/* TX 特征发送(notify,按协商 MTU 分片);返回 false = 未送出 */
bool app_ble_gatts_notify(const uint8_t *data, size_t len, void *ctx);

/* CMD TX 特征发送(命令回包,按协商 MTU 分片)。
 * 返回实际发送字节数；<0 表示未连接/未开始/参数错误。
 * 返回 0 或小于 len 表示失败/只送出前缀，调用方不能当作完整成功。 */
int app_ble_gatts_cmd_notify(const uint8_t *data, size_t len);

bool app_ble_gatts_connected(void);
uint16_t app_ble_gatts_mtu(void);

/* ---- 内部(NimBLE 单 GAP 回调模型,1.3-56)----
 * NimBLE 只有一条全局 GAP 回调(ble_hs_cfg.gap_cb,由 bt.c 注册),连接/断开/
 * 订阅/MTU 事件由 bt.c 转发到本模块;Bluedroid 构建不使用(各自回调体系)。
 * 仅 bt_gatts_nimble.c 实现这些入口。 */
void app_ble_gatts_nimble_on_sync(void);
void app_ble_gatts_nimble_on_connect(uint16_t conn_handle);
void app_ble_gatts_nimble_on_disconnect(int reason);
void app_ble_gatts_nimble_on_subscribe(uint16_t conn_handle, uint16_t attr_handle,
                                       uint8_t cur_notify);
void app_ble_gatts_nimble_on_mtu(uint16_t mtu);
