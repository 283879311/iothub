#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"

esp_err_t app_bt_apply_config(void);

/* 开机在配置加载后、app_bt_apply_config 前调用一次:释放未选模式的栈内存。
 * 任何蓝牙配置修改均整机重启生效——栈不支持运行期热重配(问题.md 第 21 项)。 */
void app_bt_release_unused_memory(void);

/* 读取蓝牙运行态快照(BT 栈回调线程与 Web 线程并发访问,内部加锁)。
 * runtime_mode/last_error_buf 任一可为 NULL 表示不取该项。 */
void app_bt_get_status(uint8_t *runtime_mode, char *last_error_buf, size_t last_error_buf_size);
