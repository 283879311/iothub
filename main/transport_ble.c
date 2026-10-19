#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_check.h"
#include "esp_log.h"

#include "bt_gattc.h"
#include "bt_gatts.h"
#include "data_pipe.h"
#include "transport_ble.h"

static const char *TAG = "ble_data";

static bool s_ready = false;

esp_err_t app_ble_dataplane_start(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    /* 出站:B port 的 write 函数 = GATT notify(在 data_pipe 任务里执行) */
    ESP_RETURN_ON_ERROR(data_pipe_register_port(DATA_PORT_BLE, app_ble_gatts_notify, NULL),
                        TAG, "register ble port failed");
    ESP_RETURN_ON_ERROR(app_ble_gatts_start(), TAG, "ble gatts start failed");
    s_ready = true;
    ESP_LOGI(TAG, "BLE data plane started (peripheral: notify)");
    return ESP_OK;
}

esp_err_t app_ble_dataplane_start_central(void)
{
    if (s_ready) {
        return ESP_OK;
    }
    /* 主角色:出站 = 写对端 RX 特征 */
    ESP_RETURN_ON_ERROR(data_pipe_register_port(DATA_PORT_BLE, app_ble_gattc_write, NULL),
                        TAG, "register ble port failed");
    ESP_RETURN_ON_ERROR((esp_err_t)app_ble_gattc_start(), TAG, "ble gattc start failed");
    s_ready = true;
    ESP_LOGI(TAG, "BLE data plane started (central: write to peer RX)");
    return ESP_OK;
}

void app_ble_dataplane_reset(void)
{
    app_ble_gatts_reset();
    app_ble_gattc_reset();
    s_ready = false;
}

void app_ble_dataplane_on_write(const uint8_t *data, size_t len)
{
    /* GATT 回调上下文:只入队,绝不阻塞(官方硬约束) */
    (void)data_pipe_feed(DATA_PORT_BLE, data, len);
}

bool app_ble_dataplane_ready(void)
{
    return s_ready;
}
