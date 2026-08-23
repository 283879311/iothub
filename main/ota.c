#include <stdbool.h>
#include <stdio.h>

#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "constants.h"
#include "http_utils.h"
#include "ota.h"

static const char *TAG = "ota";

esp_err_t app_ota_mount_fs(bool format_if_mount_failed)
{
    esp_vfs_littlefs_conf_t conf = {
        .base_path = APP_BASE_PATH,
        .partition_label = APP_STORAGE_PARTITION_LABEL,
        .format_if_mount_failed = format_if_mount_failed,
        .dont_mount = false,
    };
    esp_err_t err = esp_vfs_littlefs_register(&conf);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LittleFS mount failed: %s", esp_err_to_name(err));
        return err;
    }
    return ESP_OK;
}

void app_ota_get_storage_stats(size_t *total_bytes, size_t *used_bytes)
{
    size_t total = 0;
    size_t used = 0;

    if (esp_littlefs_info(APP_STORAGE_PARTITION_LABEL, &total, &used) != ESP_OK) {
        total = 0;
        used = 0;
    }

    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        *used_bytes = used;
    }
}

void app_ota_get_running_partition_stats(const esp_partition_t **running_partition,
                                         size_t *total_bytes,
                                         size_t *used_bytes)
{
    const esp_partition_t *partition = esp_ota_get_running_partition();
    size_t total = 0;
    size_t used = 0;

    if (partition != NULL) {
        esp_image_metadata_t metadata = {0};
        const esp_partition_pos_t part_pos = {
            .offset = partition->address,
            .size = partition->size,
        };
        total = partition->size;
        if (esp_image_get_metadata(&part_pos, &metadata) == ESP_OK) {
            used = metadata.image_len;
        }
    }

    if (running_partition != NULL) {
        *running_partition = partition;
    }
    if (total_bytes != NULL) {
        *total_bytes = total;
    }
    if (used_bytes != NULL) {
        *used_bytes = used;
    }
}

void app_ota_confirm_running_image(void)
{
#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
    const esp_partition_t *running_partition = esp_ota_get_running_partition();
    esp_ota_img_states_t ota_state = ESP_OTA_IMG_UNDEFINED;
    esp_err_t err;

    if (running_partition == NULL) {
        ESP_LOGW(TAG, "OTA verify skipped: running partition unavailable");
        return;
    }

    err = esp_ota_get_state_partition(running_partition, &ota_state);
    if (err != ESP_OK) {
        ESP_LOGW(TAG,
                 "OTA verify skipped: label=%s offset=0x%08lx state query failed: %s",
                 running_partition->label,
                 (unsigned long)running_partition->address,
                 esp_err_to_name(err));
        return;
    }

    ESP_LOGI(TAG,
             "Running OTA image: label=%s offset=0x%08lx state=%d",
             running_partition->label,
             (unsigned long)running_partition->address,
             (int)ota_state);

    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        err = esp_ota_mark_app_valid_cancel_rollback();
        if (err == ESP_OK) {
            ESP_LOGI(TAG,
                     "OTA image marked valid: label=%s offset=0x%08lx",
                     running_partition->label,
                     (unsigned long)running_partition->address);
        } else {
            ESP_LOGE(TAG,
                     "OTA image mark valid failed: label=%s offset=0x%08lx err=%s",
                     running_partition->label,
                     (unsigned long)running_partition->address,
                     esp_err_to_name(err));
        }
    }
#else
    ESP_LOGW(TAG, "Bootloader rollback is disabled; OTA auto-rollback is unavailable");
#endif
}

void app_reboot_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(APP_OTA_REBOOT_DELAY_MS));
    esp_restart();
}

static void app_ota_schedule_reboot(const char *task_name)
{
    if (xTaskCreate(app_reboot_task, task_name, 2048, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "Failed to schedule reboot task: %s", task_name);
    }
}

esp_err_t app_ota_handle_firmware_upload(httpd_req_t *req, char *scratch, size_t scratch_size)
{
    const esp_partition_t *update_partition;
    esp_ota_handle_t update_handle = 0;
    int remaining = req->content_len;
    size_t total_received = 0;
    esp_err_t err;

    if (remaining <= 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"empty_upload\"}");
    }
    if (scratch == NULL || scratch_size == 0) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"ota_scratch_unavailable\"}");
    }

    update_partition = esp_ota_get_next_update_partition(NULL);
    if (update_partition == NULL) {
        ESP_LOGE(TAG, "OTA upload rejected: no update partition available");
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"no_update_partition\"}");
    }

    ESP_LOGI(TAG,
             "OTA upload started: size=%d target_label=%s target_offset=0x%08lx target_size=%lu",
             req->content_len,
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_partition->size);

    err = esp_ota_begin(update_partition, OTA_WITH_SEQUENTIAL_WRITES, &update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_ota_begin failed: label=%s offset=0x%08lx err=%s",
                 update_partition->label,
                 (unsigned long)update_partition->address,
                 esp_err_to_name(err));
        return app_http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"esp_ota_begin_failed:%s\"}",
                               esp_err_to_name(err));
    }
    ESP_LOGI(TAG,
             "esp_ota_begin ok: label=%s offset=0x%08lx handle=%lu",
             update_partition->label,
             (unsigned long)update_partition->address,
             (unsigned long)update_handle);

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, scratch,
                                      remaining > (int)scratch_size ? (int)scratch_size : remaining);
        if (recv_len <= 0) {
            ESP_LOGE(TAG,
                     "OTA receive failed: label=%s received=%lu remaining=%d",
                     update_partition->label,
                     (unsigned long)total_received,
                     remaining);
            esp_ota_abort(update_handle);
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"ota_recv_failed\"}");
        }
        err = esp_ota_write(update_handle, scratch, recv_len);
        if (err != ESP_OK) {
            ESP_LOGE(TAG,
                     "esp_ota_write failed: label=%s received=%lu chunk=%d err=%s",
                     update_partition->label,
                     (unsigned long)total_received,
                     recv_len,
                     esp_err_to_name(err));
            esp_ota_abort(update_handle);
            return app_http_send_jsonf(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"esp_ota_write_failed:%s\"}",
                                   esp_err_to_name(err));
        }
        remaining -= recv_len;
        total_received += (size_t)recv_len;
    }

    ESP_LOGI(TAG,
             "OTA receive complete: label=%s received=%lu",
             update_partition->label,
             (unsigned long)total_received);

    err = esp_ota_end(update_handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_ota_end failed: label=%s received=%lu err=%s",
                 update_partition->label,
                 (unsigned long)total_received,
                 esp_err_to_name(err));
        esp_ota_abort(update_handle);
        return app_http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"esp_ota_end_failed:%s\"}",
                               esp_err_to_name(err));
    }
    ESP_LOGI(TAG,
             "esp_ota_end ok: label=%s received=%lu",
             update_partition->label,
             (unsigned long)total_received);

    err = esp_ota_set_boot_partition(update_partition);
    if (err != ESP_OK) {
        ESP_LOGE(TAG,
                 "esp_ota_set_boot_partition failed: label=%s offset=0x%08lx err=%s",
                 update_partition->label,
                 (unsigned long)update_partition->address,
                 esp_err_to_name(err));
        return app_http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"set_boot_partition_failed:%s\"}",
                               esp_err_to_name(err));
    }
    ESP_LOGI(TAG,
             "esp_ota_set_boot_partition ok: label=%s offset=0x%08lx",
             update_partition->label,
             (unsigned long)update_partition->address);

    app_http_send_json_text(req, NULL,
                        "{\"status\":\"ok\",\"message\":\"ota image received, device will reboot\"}");
    app_ota_schedule_reboot("ota_reboot");
    return ESP_OK;
}

esp_err_t app_ota_handle_storage_upload(httpd_req_t *req, char *scratch, size_t scratch_size)
{
    const esp_partition_t *storage_partition;
    size_t erase_size;
    size_t offset = 0;
    int remaining = req->content_len;
    esp_err_t err;
    bool unmounted = false;

    if (remaining <= 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"empty_upload\"}");
    }
    if (scratch == NULL || scratch_size == 0) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"storage_scratch_unavailable\"}");
    }

    storage_partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                 ESP_PARTITION_SUBTYPE_ANY,
                                                 APP_STORAGE_PARTITION_LABEL);
    if (storage_partition == NULL) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"storage_partition_not_found\"}");
    }
    if ((size_t)remaining > storage_partition->size) {
        return app_http_send_jsonf(req, "413 Payload Too Large",
                               "{\"status\":\"error\",\"message\":\"storage_image_too_large\",\"max_size\":%u}",
                               (unsigned)storage_partition->size);
    }

    err = esp_vfs_littlefs_unregister(APP_STORAGE_PARTITION_LABEL);
    if (err != ESP_OK) {
        return app_http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"storage_unmount_failed:%s\"}",
                               esp_err_to_name(err));
    }
    unmounted = true;

    erase_size = ((size_t)req->content_len + APP_FLASH_SECTOR_SIZE - 1) / APP_FLASH_SECTOR_SIZE;
    erase_size *= APP_FLASH_SECTOR_SIZE;
    err = esp_partition_erase_range(storage_partition, 0, erase_size);
    if (err != ESP_OK) {
        if (unmounted) {
            app_ota_mount_fs(false);
        }
        return app_http_send_jsonf(req, "500 Internal Server Error",
                               "{\"status\":\"error\",\"message\":\"storage_erase_failed:%s\"}",
                               esp_err_to_name(err));
    }

    while (remaining > 0) {
        int recv_len = httpd_req_recv(req, scratch,
                                      remaining > (int)scratch_size ? (int)scratch_size : remaining);
        if (recv_len <= 0) {
            if (unmounted) {
                app_ota_mount_fs(false);
            }
            return app_http_send_json_text(req, "400 Bad Request",
                                       "{\"status\":\"error\",\"message\":\"storage_recv_failed\"}");
        }
        err = esp_partition_write(storage_partition, offset, scratch, recv_len);
        if (err != ESP_OK) {
            if (unmounted) {
                app_ota_mount_fs(false);
            }
            return app_http_send_jsonf(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"storage_write_failed:%s\"}",
                                   esp_err_to_name(err));
        }
        remaining -= recv_len;
        offset += (size_t)recv_len;
    }

    app_http_send_json_text(req, NULL,
                        "{\"status\":\"ok\",\"message\":\"storage image received, device will reboot\"}");
    app_ota_schedule_reboot("storage_reboot");
    return ESP_OK;
}
