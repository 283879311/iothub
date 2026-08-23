#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_http_server.h"
#include "esp_partition.h"

esp_err_t app_ota_mount_fs(bool format_if_mount_failed);
void app_ota_confirm_running_image(void);
void app_ota_get_storage_stats(size_t *total_bytes, size_t *used_bytes);
void app_ota_get_running_partition_stats(const esp_partition_t **running_partition,
                                         size_t *total_bytes,
                                         size_t *used_bytes);
void app_reboot_task(void *arg);
esp_err_t app_ota_handle_firmware_upload(httpd_req_t *req, char *scratch, size_t scratch_size);
esp_err_t app_ota_handle_storage_upload(httpd_req_t *req, char *scratch, size_t scratch_size);
