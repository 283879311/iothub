#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_http_server.h"
#include "esp_image_format.h"
#include "esp_littlefs.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "constants.h"
#include "http_utils.h"
#include "mqtt.h"
#include "ota.h"
#include "wifi.h"

static const char *TAG = "ota";

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/* 回滚确认的健康窗口:纯 STA 模式 90s 连不上会自动降级为 AP 并持久化,
 * 所以正常固件在窗口内必然拿到 sta_ip 或 ap_up;窗口耗尽说明 WiFi 栈整体失效,回滚。 */
#define APP_OTA_CONFIRM_MIN_DELAY_MS 15000
#define APP_OTA_CONFIRM_WINDOW_MS 120000
#define APP_OTA_CONFIRM_POLL_MS 1000
#endif

/* 新固件应用描述区在镜像内的偏移:esp_image_header_t(24) + esp_image_segment_header_t(8) */
#define APP_OTA_APP_DESC_OFFSET 32
#define APP_OTA_IMAGE_MIN_HEADER (APP_OTA_APP_DESC_OFFSET + sizeof(esp_app_desc_t))

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

#if CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE
/* 健康判定:STA 拿到 IP、AP 网络面已激活、或 MQTT 已连上,任一满足即认为新固件业务可用。 */
static bool app_ota_network_healthy(const char **reason)
{
    bool sta_has_ip;
    bool mqtt_connected;

    app_wifi_runtime_lock();
    sta_has_ip = s_wifi.sta_has_ip;
    app_wifi_runtime_unlock();
    if (sta_has_ip) {
        *reason = "sta_ip";
        return true;
    }
    if (s_ap_netif != NULL && esp_netif_is_netif_up(s_ap_netif)) {
        *reason = "ap_up";
        return true;
    }
    app_mqtt_lock();
    mqtt_connected = s_mqtt.connected;
    app_mqtt_unlock();
    if (mqtt_connected) {
        *reason = "mqtt_connected";
        return true;
    }
    return false;
}

static void app_ota_confirm_task(void *arg)
{
    TickType_t waited_ms;

    (void)arg;
    /* 最低观察期:即使启动即健康也先跑一会儿,覆盖启动完成数秒后才触发的崩溃 */
    vTaskDelay(pdMS_TO_TICKS(APP_OTA_CONFIRM_MIN_DELAY_MS));
    waited_ms = APP_OTA_CONFIRM_MIN_DELAY_MS;
    while (waited_ms < APP_OTA_CONFIRM_WINDOW_MS) {
        const char *reason = NULL;
        if (app_ota_network_healthy(&reason)) {
            esp_err_t err = esp_ota_mark_app_valid_cancel_rollback();
            if (err == ESP_OK) {
                ESP_LOGI(TAG, "OTA image confirmed valid after %us (reason=%s)",
                         (unsigned)(waited_ms / 1000), reason);
            } else {
                ESP_LOGE(TAG, "OTA mark valid failed: %s", esp_err_to_name(err));
            }
            vTaskDelete(NULL);
            return;
        }
        vTaskDelay(pdMS_TO_TICKS(APP_OTA_CONFIRM_POLL_MS));
        waited_ms += APP_OTA_CONFIRM_POLL_MS;
    }
    ESP_LOGE(TAG, "OTA image unhealthy: no sta_ip/ap_up/mqtt within %us, rolling back",
             (unsigned)(APP_OTA_CONFIRM_WINDOW_MS / 1000));
    esp_ota_mark_app_invalid_rollback_and_reboot();
}
#endif

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

    /* 仅 OTA 升级后的镜像处于 PENDING_VERIFY;常规启动(含回滚后的旧镜像)直接跳过。
     * 确认延迟到独立任务:健康(业务可达)才 mark valid,超时则主动回滚重启。 */
    if (ota_state == ESP_OTA_IMG_PENDING_VERIFY) {
        if (xTaskCreate(app_ota_confirm_task, "ota_confirm", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGE(TAG, "Failed to create ota_confirm task; image stays PENDING_VERIFY "
                          "(will roll back on next reboot)");
        } else {
            ESP_LOGI(TAG, "OTA image pending verify: health confirmation started "
                          "(min %ums, window %ums)",
                     (unsigned)APP_OTA_CONFIRM_MIN_DELAY_MS,
                     (unsigned)APP_OTA_CONFIRM_WINDOW_MS);
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

static void app_ota_copy_bounded(char *dst, size_t dst_size, const char *src, size_t src_size)
{
    size_t i;

    for (i = 0; i + 1 < dst_size && i < src_size && src[i] != '\0'; i++) {
        dst[i] = src[i];
    }
    dst[i] = '\0';
}

/* 逐段数值比较 "N.N.N" 形式的版本号,段数不足按 0 处理("1.2" < "1.10")。 */
static bool app_ota_version_at_least(const char *candidate, const char *baseline)
{
    const char *p = candidate;
    const char *q = baseline;

    while (*p != '\0' || *q != '\0') {
        unsigned long va = 0;
        unsigned long vb = 0;
        bool pa = false;
        bool pb = false;

        while (*p != '\0' && (*p < '0' || *p > '9')) {
            p++;
        }
        while (*q != '\0' && (*q < '0' || *q > '9')) {
            q++;
        }
        while (*p >= '0' && *p <= '9') {
            va = va * 10UL + (unsigned long)(*p - '0');
            p++;
            pa = true;
        }
        while (*q >= '0' && *q <= '9') {
            vb = vb * 10UL + (unsigned long)(*q - '0');
            q++;
            pb = true;
        }
        if (!pa && !pb) {
            break;
        }
        if (va != vb) {
            return va > vb;
        }
    }
    return true;
}

/* 校验上传镜像头:应用描述区魔数、项目名、版本(仅拒绝降级,可用查询参数豁免)。
 * 校验失败时 abort OTA 并已发送响应,调用方直接返回其 err。 */
static esp_err_t app_ota_validate_upload_header(httpd_req_t *req,
                                                esp_ota_handle_t update_handle,
                                                const char *scratch,
                                                bool allow_downgrade)
{
    const esp_app_desc_t *upload_desc;
    const esp_app_desc_t *running_desc;
    char running_version[sizeof(((esp_app_desc_t *)0)->version)];
    char running_project[sizeof(((esp_app_desc_t *)0)->project_name)];
    char upload_version[sizeof(running_version)];
    char upload_project[sizeof(running_project)];

    upload_desc = (const esp_app_desc_t *)(scratch + APP_OTA_APP_DESC_OFFSET);
    running_desc = esp_app_get_description();

    if (upload_desc->magic_word != ESP_APP_DESC_MAGIC_WORD) {
        ESP_LOGE(TAG, "OTA upload rejected: bad app desc magic 0x%08lx",
                 (unsigned long)upload_desc->magic_word);
        esp_ota_abort(update_handle);
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"ota_image_invalid\"}");
    }
    app_ota_copy_bounded(upload_version, sizeof(upload_version), upload_desc->version, sizeof(upload_desc->version));
    app_ota_copy_bounded(upload_project, sizeof(upload_project), upload_desc->project_name, sizeof(upload_desc->project_name));
    if (running_desc != NULL) {
        app_ota_copy_bounded(running_version, sizeof(running_version), running_desc->version, sizeof(running_desc->version));
        app_ota_copy_bounded(running_project, sizeof(running_project), running_desc->project_name, sizeof(running_desc->project_name));
    } else {
        running_version[0] = '\0';
        running_project[0] = '\0';
    }

    if (running_project[0] != '\0' && upload_project[0] != '\0' &&
        strcmp(running_project, upload_project) != 0) {
        ESP_LOGE(TAG, "OTA upload rejected: project mismatch running='%s' upload='%s'",
                 running_project, upload_project);
        esp_ota_abort(update_handle);
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"ota_project_mismatch\"}");
    }
    if (!allow_downgrade &&
        running_version[0] != '\0' && upload_version[0] != '\0' &&
        !app_ota_version_at_least(upload_version, running_version)) {
        ESP_LOGE(TAG, "OTA upload rejected: downgrade running='%s' upload='%s' "
                      "(append ?allow_downgrade=1 to override)",
                 running_version, upload_version);
        esp_ota_abort(update_handle);
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"ota_version_older\"}");
    }
    ESP_LOGI(TAG, "OTA image header ok: version='%s' project='%s'", upload_version, upload_project);
    return ESP_OK;
}

esp_err_t app_ota_handle_firmware_upload(httpd_req_t *req, char *scratch, size_t scratch_size)
{
    const esp_partition_t *update_partition;
    esp_ota_handle_t update_handle = 0;
    int remaining = req->content_len;
    size_t total_received = 0;
    bool image_checked = false;
    bool allow_downgrade = false;
    esp_err_t err;

    if (remaining <= 0) {
        return app_http_send_json_text(req, "400 Bad Request",
                                   "{\"status\":\"error\",\"message\":\"empty_upload\"}");
    }
    if (scratch == NULL || scratch_size == 0) {
        return app_http_send_json_text(req, "500 Internal Server Error",
                                   "{\"status\":\"error\",\"message\":\"ota_scratch_unavailable\"}");
    }

    {
        char query[64];
        char param[8];
        if (httpd_req_get_url_query_str(req, query, sizeof(query)) == ESP_OK &&
            httpd_query_key_value(query, "allow_downgrade", param, sizeof(param)) == ESP_OK &&
            strcmp(param, "1") == 0) {
            allow_downgrade = true;
        }
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
        if (!image_checked) {
            /* 首块必须包含完整应用描述区,用于魔数/项目名/版本校验 */
            if ((size_t)recv_len < APP_OTA_IMAGE_MIN_HEADER) {
                ESP_LOGE(TAG, "OTA upload rejected: first chunk too small (%d bytes)", recv_len);
                esp_ota_abort(update_handle);
                return app_http_send_json_text(req, "400 Bad Request",
                                           "{\"status\":\"error\",\"message\":\"ota_image_invalid\"}");
            }
            err = app_ota_validate_upload_header(req, update_handle, scratch, allow_downgrade);
            if (err != ESP_OK) {
                return err;
            }
            image_checked = true;
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
