#pragma once

#include <stdbool.h>

#include "esp_err.h"

/* BLE 配网(官方 network_provisioning 组件 + BLE transport)
 *
 * 定位:仅在"项目自身没有任何 Wi-Fi 凭据"时进入配网态;与现有 SoftAP 强制门户
 * 并存(门户负责"一次配全量",BLE 配网负责"免切网")。
 *
 * 生命周期隔离(重要):BLE 传输运行时约 110KB 内存(官方数字),且配网组件在
 * BLE 模式下要求 wifi_mode=STA,因此配网态与"数据面运行态"互斥 ——
 * 配网成功即把凭据写入项目自身的 wifi_profiles 并重启进入运行态。 */

/* 是否需要进入配网态(项目自身无 profile 且无历史 STA 配置) */
bool app_prov_needed(void);

/* 启动配网:BLE peripheral + Sec1(X25519+AES-CTR) + PoP,MAC 派生 6 位;
 * 设备名按官方约定 PROV_xxxxxx。成功返回 ESP_OK(随后收到凭据会自动重启)。 */
esp_err_t app_prov_start(void);

/* 主动停止配网并释放管理器(释放 BTDM 内存由 scheme 事件处理器负责) */
void app_prov_stop(void);

bool app_prov_is_active(void);

/* PoP 与配网服务名(供 Web 状态面板/产线贴标使用) */
const char *app_prov_pop(void);
const char *app_prov_service_name(void);
