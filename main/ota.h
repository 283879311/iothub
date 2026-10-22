#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_partition.h"

void app_ota_confirm_running_image(void);
void app_ota_get_running_partition_stats(const esp_partition_t **running_partition,
                                         size_t *total_bytes,
                                         size_t *used_bytes);
void app_reboot_task(void *arg);
void app_reboot_schedule(const char *task_name);
