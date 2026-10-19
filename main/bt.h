#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"

esp_err_t app_bt_apply_config(void);

/* ---- 供数据面复用的栈底座(唯一触碰 BT 栈的入口) ---- */
/* 确保 BLE 栈已就绪(幂等;失败返回错误但不 abort) */
esp_err_t app_bt_stack_ensure(void);
/* 拆栈(停止广播并释放栈资源) */
void app_bt_stack_teardown(void);

/* 开机在配置加载后、app_bt_apply_config 前调用一次:释放未选模式的栈内存。
 * 任何蓝牙配置修改均整机重启生效——栈不支持运行期热重配(问题与版本优化.md 第 21 项)。 */
void app_bt_release_unused_memory(void);

/* 显式重启 BLE 广播(问题与版本优化.md 1.3-03 加固)。
 * 背景:Bluedroid 建立连接时会停止广播、断开后**理论上**自动恢复;实测存在断开后
 * 未恢复、设备彻底不可见的情形(手机侧 App 崩溃导致 REMOTE_USER_TERMINATED=0x13 断链)。
 * 故在 GATTS 断开事件里显式重启一次;未配置广播(非 BLE 模式)或已在广播时按正常处理。 */
esp_err_t app_bt_restart_advertising(void);

/* 当前是否处于广播中(由 GAP 的 START/STOP complete 事件维护) ——
 * 用于在主机侧一眼判断"设备是否可见",避免"断开后静默不广播"再次难查(1.3-03)。 */
bool app_bt_advertising(void);

/* 读取蓝牙运行态快照(BT 栈回调线程与 Web 线程并发访问,内部加锁)。
 * runtime_mode/last_error_buf 任一可为 NULL 表示不取该项。 */
void app_bt_get_status(uint8_t *runtime_mode, char *last_error_buf, size_t last_error_buf_size);

/* 清除 last_error 状态串(1.3-64:central 连接建立后由 bt_gattc_nimble 调用,
 * 避免状态页长期显示 "central_scanning"/"ble_starting") */
void app_bt_status_clear_error(void);
