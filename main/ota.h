#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_partition.h"

void app_ota_confirm_running_image(void);
void app_ota_get_running_partition_stats(const esp_partition_t **running_partition,
                                         size_t *total_bytes,
                                         size_t *used_bytes);
void app_reboot_task(void *arg);
typedef enum {
    APP_REBOOT_SCHEDULED = 0,       /* 本次成功创建复位任务 */
    APP_REBOOT_ALREADY_PENDING,     /* 已有复位任务在途 */
    APP_REBOOT_SCHEDULE_FAILED,     /* 任务创建失败,未调度 */
} app_reboot_result_t;

app_reboot_result_t app_reboot_schedule(const char *task_name);
