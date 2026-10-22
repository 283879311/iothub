#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_app_desc.h"
#include "esp_image_format.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_system.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "config.h"
#include "constants.h"
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

/* 重启调度(1.3-40):各调用方(BT 配置保存/云端命令/BLE 命令/恢复出厂)都在
 * 低堆场景下触发 —— MQTT 连接风暴能把 largest 砸到 ~1.5KB,届时 xTaskCreate
 * (2048 栈 + TCB)会静默失败,表现为"已保存将重启"但设备不重启,配置落盘了
 * 却不生效(实测 BLE 角色切换连续两次没重启)。改静态任务:创建零堆分配、
 * 不可能失败;pending 位防并发调用重复抢占同一 StaticTask_t。
 * 1.3-53 栈溢出修复:IDF 6.0.2 Xtensa 的 StackType_t = uint8_t(portmacro.h:78),
 * 即 xTaskCreateStatic 的栈深度单位是**字节**而非"字"!原 512 实际只有 512B,
 * esp_restart 的复位链直接溢出 —— coredump 实证 panic "stack overflow in
 * task bt_config_reboo"(溢出后设备仍经 panic 路径重启,所以 1.3-40 的验收
 * "一次生效"没暴露,但页面侧表现为保存瞬间崩溃)。扩到 4096B(4KB):重启任务
 * 只存活 ~1.5s,静态栈 +3.5KB BSS 换重启路径在最坏情况下也绝不溢出。 */
#define APP_OTA_REBOOT_STACK_BYTES 4096
static StaticTask_t s_reboot_task_buf;
static StackType_t s_reboot_stack[APP_OTA_REBOOT_STACK_BYTES];
static bool s_reboot_pending = false;

void app_reboot_schedule(const char *task_name)
{
    if (s_reboot_pending) {
        return;   /* 已有重启在途,esp_restart 不会回来 */
    }
    s_reboot_pending = true;
    (void)xTaskCreateStatic(app_reboot_task, task_name, APP_OTA_REBOOT_STACK_BYTES,
                            NULL, 5, s_reboot_stack, &s_reboot_task_buf);
}

