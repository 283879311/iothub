#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_system.h"
#include "esp_wifi_types.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "network_provisioning/manager.h"
#include "network_provisioning/scheme_ble.h"

#include "ble_prov.h"
#include "bt.h"
#include "common.h"
#include "config.h"
#include "constants.h"

static const char *TAG = "ble_prov";

/* 项目全局配置(与 mqtt.c / bt.c 同一惯例:直接 extern) */
extern app_config_t s_config;

#define PROV_POP_CHARS          6
#define PROV_SERVICE_NAME_LEN   16
#define PROV_REBOOT_DELAY_MS    1500
#define PROV_RESTART_TASK_STACK 3072

static bool s_active = false;
static bool s_restart_scheduled = false;
static char s_pop[PROV_POP_CHARS + 1];
static char s_service_name[PROV_SERVICE_NAME_LEN];

/* ---------------------------------------------------------------------------
 * 身份:服务名 PROV_xxxxxx(官方约定)+ PoP(每设备唯一,可打印贴标)
 * ------------------------------------------------------------------------- */
static void app_prov_build_identity(void)
{
    uint8_t mac[6] = {0};
    unsigned seed;

    if (esp_read_mac(mac, ESP_MAC_WIFI_STA) != ESP_OK) {
        memset(mac, 0, sizeof(mac));
    }
    snprintf(s_service_name, sizeof(s_service_name), "PROV_%02X%02X%02X",
             mac[3], mac[4], mac[5]);
    seed = ((unsigned)mac[3] << 16) | ((unsigned)mac[4] << 8) | (unsigned)mac[5];
    snprintf(s_pop, sizeof(s_pop), "%06u", seed % 1000000u);
}

const char *app_prov_pop(void) { return s_pop; }

const char *app_prov_service_name(void) { return s_service_name; }

bool app_prov_is_active(void) { return s_active; }

/* ---------------------------------------------------------------------------
 * 是否需要配网:以"项目自身的凭据存储"为准(兼容由 SoftAP 门户配过的老设备)
 * ------------------------------------------------------------------------- */
bool app_prov_needed(void)
{
    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX] = {0};
    size_t count = 0;
    bool legacy_sta;

    if (app_config_load_wifi_profiles(profiles, APP_WIFI_PROFILE_MAX, &count) == ESP_OK && count > 0) {
        return false;
    }
    app_config_lock();
    legacy_sta = (s_config.sta_ssid[0] != '\0');
    app_config_unlock();
    return !legacy_sta;
}

/* ---------------------------------------------------------------------------
 * 配网成功 → 延时重启进入运行态(顺带释放配网用的内存)
 * ------------------------------------------------------------------------- */
static void app_prov_restart_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(PROV_REBOOT_DELAY_MS));
    esp_restart();
}

static void app_prov_schedule_restart(void)
{
    if (s_restart_scheduled) {
        return;
    }
    s_restart_scheduled = true;
    if (xTaskCreate(app_prov_restart_task, "prov_restart", PROV_RESTART_TASK_STACK,
                    NULL, 5, NULL) != pdPASS) {
        ESP_LOGW(TAG, "restart task create failed, restart now");
        esp_restart();
    }
}

/* ---------------------------------------------------------------------------
 * 配网事件:
 *   CRED_RECV   → 把凭据写进项目自身的 profile 列表(与选网逻辑同一条存储)
 *   CRED_FAIL   → 记录原因(不重启,允许用户重新下发)
 *   CRED_SUCCESS→ 写盘已完成 → 延时重启进入运行态
 * ------------------------------------------------------------------------- */
static void app_prov_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    (void)arg;

    if (base != NETWORK_PROV_EVENT) {
        return;
    }

    switch (id) {
    case NETWORK_PROV_WIFI_CRED_RECV: {
        const wifi_sta_config_t *sta = (const wifi_sta_config_t *)data;

        if (sta == NULL) {
            break;
        }
        ESP_LOGI(TAG, "credentials received: ssid='%s'", (const char *)sta->ssid);
        if (app_config_save_wifi_profile((const char *)sta->ssid,
                                        (const char *)sta->password) != ESP_OK) {
            ESP_LOGE(TAG, "save wifi profile failed");
        }
        break;
    }
    case NETWORK_PROV_WIFI_CRED_FAIL: {
        network_prov_wifi_sta_fail_reason_t reason = (network_prov_wifi_sta_fail_reason_t)0;

        if (network_prov_mgr_get_wifi_disconnect_reason(&reason) == ESP_OK) {
            ESP_LOGW(TAG, "wi-fi connect failed, reason=%d (check credentials)", (int)reason);
        } else {
            ESP_LOGW(TAG, "wi-fi connect failed (check credentials)");
        }
        break;
    }
    case NETWORK_PROV_WIFI_CRED_SUCCESS:
        ESP_LOGI(TAG, "provisioning success (ssid saved), restarting into normal mode");
        app_prov_schedule_restart();
        break;
    case NETWORK_PROV_END:
        ESP_LOGI(TAG, "provisioning service stopped");
        break;
    case NETWORK_PROV_DEINIT:
        ESP_LOGI(TAG, "provisioning manager deinitialized");
        break;
    default:
        break;
    }
}

/* ---------------------------------------------------------------------------
 * 启动配网
 * 前置:默认事件循环与 Wi-Fi 栈已就绪(main.c 在 app_start_wifi() 之后调用)
 * ------------------------------------------------------------------------- */
esp_err_t app_prov_start(void)
{
    network_prov_mgr_config_t mgr_cfg = {
        .scheme = network_prov_scheme_ble,
        /* 配网结束即释放 BTDM 内存 —— 与"配网态/运行态互斥"一致(官方 ~110KB) */
        .scheme_event_handler = NETWORK_PROV_SCHEME_BLE_EVENT_HANDLER_FREE_BTDM,
    };
    bool already_provisioned = false;

    if (s_active) {
        return ESP_OK;
    }

    app_prov_build_identity();
    ESP_LOGI(TAG, "start BLE provisioning: name='%s' pop='%s' security=1(X25519+PoP)",
             s_service_name, s_pop);

    /* 1) bt.c 让位:不启动自有广播,但复用其栈底座(唯一触碰 BT 栈的入口) */
    app_bt_set_provisioning_active(true);
    ESP_RETURN_ON_ERROR(app_bt_stack_ensure(), TAG, "bt stack ensure failed");

    /* 2) 初始化配网管理器(BLE transport) */
    ESP_RETURN_ON_ERROR(network_prov_mgr_init(mgr_cfg), TAG, "prov mgr init failed");

    /* 3) 组件侧已配网则立即退出(与 app_prov_needed() 双重保险) */
    if (network_prov_mgr_is_wifi_provisioned(&already_provisioned) == ESP_OK && already_provisioned) {
        ESP_LOGW(TAG, "already provisioned (component state), skip provisioning");
        network_prov_mgr_deinit();
        app_bt_set_provisioning_active(false);
        return ESP_ERR_INVALID_STATE;
    }

    /* 4) 注册应用事件处理(必须在 start 之前,避免错过早期事件) */
    ESP_RETURN_ON_ERROR(esp_event_handler_register(NETWORK_PROV_EVENT, ESP_EVENT_ANY_ID,
                                                   &app_prov_event_handler, NULL),
                        TAG, "register prov event handler failed");

    /* 5) 不由管理器自动收尾:我们自己在 CRED_RECV/CRED_SUCCESS 里收尾 */
    ESP_RETURN_ON_ERROR(network_prov_mgr_disable_auto_stop(0), TAG, "disable auto stop failed");

    /* 6) 启动:Sec1 = X25519 + AES-CTR + PoP;service_key 仅 SoftAP 传输使用 */
    ESP_RETURN_ON_ERROR(network_prov_mgr_start_provisioning(NETWORK_PROV_SECURITY_1,
                                                            (const void *)s_pop,
                                                            s_service_name, NULL),
                        TAG, "start provisioning failed");

    s_active = true;
    ESP_LOGI(TAG, "BLE provisioning started: scan for '%s' (PoP=%s)", s_service_name, s_pop);
    return ESP_OK;
}

void app_prov_stop(void)
{
    if (!s_active) {
        return;
    }
    network_prov_mgr_stop_provisioning();
    network_prov_mgr_deinit();
    app_bt_set_provisioning_active(false);
    s_active = false;
    ESP_LOGI(TAG, "provisioning stopped");
}
