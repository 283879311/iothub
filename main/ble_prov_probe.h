#pragma once

#include <stdbool.h>

/* 临时 BLE 配网探针：只在 CONFIG_IOTHUB_BLE_PROV_PROBE=y 的分支构建中启用。
 * 未配网时由官方 network_provisioning 独占 Wi-Fi/NimBLE；成功后写入
 * app_config_t 并重启，正常启动继续走现有固件路径。 */
bool app_ble_prov_probe_should_start(void);
void app_ble_prov_probe_run(void);
