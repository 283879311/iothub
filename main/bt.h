#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

#include "config.h"

esp_err_t app_bt_apply_config(void);
void app_bt_restart_task(void *arg);

/* 开机在配置加载后、app_bt_apply_config 前调用一次:释放未选模式的栈内存 */
void app_bt_release_unused_memory(void);
/* 跨模式切换需要整机重启(未选模式内存已释放);同模式修改可热重配 */
bool app_bt_mode_switch_requires_reboot(uint8_t new_mode);

/* 读取蓝牙运行态快照(BT 栈回调线程与 Web 线程并发访问,内部加锁)。
 * runtime_mode/last_error_buf 任一可为 NULL 表示不取该项。 */
void app_bt_get_status(uint8_t *runtime_mode, char *last_error_buf, size_t last_error_buf_size);
