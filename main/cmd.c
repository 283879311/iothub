#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "bt.h"
#include "bt_gattc.h"
#include "bt_gatts.h"
#include "cmd.h"
#include "common.h"
#include "config.h"
#include "config_apply.h"
#include "constants.h"
#include "data_pipe.h"
#include "esp_app_desc.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "identity.h"
#include "mqtt.h"
#include "prov_pin.h"
#include "state.h"
#include "uart.h"
#include "wifi.h"

static const char *TAG = "cmd";

extern app_config_t s_config;

static const char *const k_config_domains[] = {
    "network", "mqtt", "bluetooth", "uart", "gpio", "web",
};

#define APP_NET_TRIAL_TTL_MS (300 * 1000)

typedef struct {
    bool valid;
    char id[20];
    char ssid[33];
    char password[65];
    int64_t expires_ms;
} app_cmd_net_trial_t;

static app_cmd_net_trial_t s_net_trial;

static void app_cmd_net_trial_clear(void)
{
    memset(&s_net_trial, 0, sizeof(s_net_trial));
}

static bool app_cmd_net_trial_ready(const char *trial_id)
{
    int64_t now = esp_timer_get_time() / 1000;

    if (!s_net_trial.valid || now >= s_net_trial.expires_ms) {
        if (s_net_trial.valid) {
            app_cmd_net_trial_clear();
        }
        return false;
    }
    return trial_id == NULL || strcmp(trial_id, s_net_trial.id) == 0;
}

static cJSON *app_cmd_build_network_status(void)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *trial;

    if (root == NULL) {
        return NULL;
    }
    app_wifi_runtime_lock();
    cJSON_AddBoolToObject(root, "sta_connected", s_wifi.sta_connected);
    cJSON_AddBoolToObject(root, "sta_has_ip", s_wifi.sta_has_ip);
    cJSON_AddStringToObject(root, "sta_ip", s_wifi.sta_ip);
    cJSON_AddStringToObject(root, "last_disconnect", s_wifi.last_disconnect);
    app_wifi_runtime_unlock();

    app_config_lock();
    cJSON_AddStringToObject(root, "sta_ssid", s_config.sta_ssid);
    cJSON_AddBoolToObject(root, "password_set", s_config.sta_password[0] != '\0');
    cJSON_AddStringToObject(root, "mode", app_network_mode_to_string(s_config.net_mode));
    app_config_unlock();

    app_mqtt_lock();
    cJSON_AddBoolToObject(root, "mqtt_connected", s_mqtt.connected);
    cJSON_AddStringToObject(root, "mqtt_last_error", s_mqtt.last_error);
    app_mqtt_unlock();

    trial = cJSON_AddObjectToObject(root, "trial");
    if (trial != NULL) {
        if (app_cmd_net_trial_ready(NULL)) {
            cJSON_AddStringToObject(trial, "trial_id", s_net_trial.id);
            cJSON_AddStringToObject(trial, "state", "got_ip");
            cJSON_AddStringToObject(trial, "ssid", s_net_trial.ssid);
        } else {
            cJSON_AddStringToObject(trial, "state", "idle");
        }
    }
    return root;
}

/* ---------------------------------------------------------------------------
 * 护栏:云端/BLE 命令面可改网络参数的开关 —— 默认关闭(改错会自断通道,只能现场恢复)。
 * 开启需改此宏重编(不做远端/网页开关,防止"云端自己开自己")。1.3-31/1.3-32 共用。
 * ------------------------------------------------------------------------- */
#define APP_CLOUD_ALLOW_NET_CMDS 0

static bool app_cmd_patch_allowed(app_cmd_source_t source, const cJSON *params)
{
#if APP_CLOUD_ALLOW_NET_CMDS
    (void)source;
    (void)params;
    return true;
#else
    const cJSON *item = NULL;

    /* HTTP(配置页)和 BLE(本地配网)允许改网络/MQTT/BT/UART/管理员字段;
     * MQTT 下发默认禁止这些本地/身份相关域,避免远端改错 host/token、
     * 改 Wi-Fi、改 UART/BT 或替换管理员用户名后把自己锁在门外。
     * 注意:GPIO 控制和 data_route 仍按现有产品策略允许 MQTT。 */
    if (source != APP_CMD_SOURCE_MQTT) {
        return true;
    }
    if (!cJSON_IsObject(params)) {
        return true;   /* 非法 params 由统一层报错 */
    }
    cJSON_ArrayForEach(item, params) {
        const cfg_field_t *f = config_apply_field(item->string != NULL ? item->string : "");

        if (f != NULL &&
            (f->domain == CFG_DOMAIN_NET || f->domain == CFG_DOMAIN_MQTT ||
             f->domain == CFG_DOMAIN_BT || f->domain == CFG_DOMAIN_UART ||
             f->domain == CFG_DOMAIN_WEB_AUTH)) {
            ESP_LOGW(TAG, "remote patch rejected (guard): field '%s' domain=%d",
                     item->string, (int)f->domain);
            return false;
        }
    }
    return true;
#endif
}

static bool app_cmd_verify_web_password(const char *password)
{
    uint8_t salt[16];
    uint8_t stored[32];
    uint8_t calc[32];
    uint8_t diff = 0;
    size_t i;

    if (password == NULL) {
        return false;
    }
    app_config_lock();
    memcpy(salt, s_config.web_pass_salt, sizeof(salt));
    memcpy(stored, s_config.web_pass_hash, sizeof(stored));
    app_config_unlock();
    app_web_hash_password(salt, sizeof(salt), password, calc);
    for (i = 0; i < sizeof(calc); i++) {
        diff |= (uint8_t)(calc[i] ^ stored[i]);
    }
    return diff == 0;
}

static bool app_cmd_parse_bool_param(const cJSON *params, bool fallback)
{
    const cJSON *p = NULL;
    if (params == NULL) {
        return fallback;
    }
    if (cJSON_IsObject(params)) {
        p = cJSON_GetObjectItemCaseSensitive(params, "value");
    }
    if (p == NULL) {
        p = params;
    }
    if (cJSON_IsBool(p)) {
        return cJSON_IsTrue(p);
    }
    if (cJSON_IsNumber(p)) {
        return p->valueint != 0;
    }
    return fallback;
}

static bool app_cmd_domain_known(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(k_config_domains) / sizeof(k_config_domains[0]); i++) {
        if (strcmp(name, k_config_domains[i]) == 0) {
            return true;
        }
    }
    return false;
}

/* config.get domains 过滤:domains 省略表示全部;未知域名由调用方先拒绝 */
static bool app_cmd_domain_wanted(const cJSON *domains, const char *name)
{
    const cJSON *item = NULL;

    if (domains == NULL) {
        return true;
    }
    cJSON_ArrayForEach(item, domains) {
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            strcmp(item->valuestring, name) == 0) {
            return true;
        }
    }
    return false;
}

/* getConfig 的脱敏汇总:与 HTTP GET 同规则 —— 口令/token 永不回明文,只回 *_set */
static cJSON *app_cmd_build_get_config(const cJSON *domains)
{
    app_config_t cfg;
    cJSON *root;
    cJSON *sec;
    const char *parity;

    app_config_lock();
    cfg = s_config;
    app_config_unlock();

    root = cJSON_CreateObject();
    if (root == NULL) {
        return NULL;
    }

    sec = cJSON_AddObjectToObject(root, "network");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "net_mode", app_network_mode_to_string(cfg.net_mode));
        cJSON_AddStringToObject(sec, "ap_ssid", cfg.ap_ssid);
        cJSON_AddBoolToObject(sec, "ap_password_set", cfg.ap_password[0] != '\0');
        cJSON_AddStringToObject(sec, "sta_ssid", cfg.sta_ssid);
        cJSON_AddBoolToObject(sec, "sta_password_set", cfg.sta_password[0] != '\0');
        cJSON_AddBoolToObject(sec, "sta_use_static", cfg.sta_use_static != 0);
        cJSON_AddStringToObject(sec, "sta_static_ip", cfg.sta_static_ip);
        cJSON_AddStringToObject(sec, "sta_static_gateway", cfg.sta_static_gateway);
        cJSON_AddStringToObject(sec, "sta_static_netmask", cfg.sta_static_netmask);
        cJSON_AddStringToObject(sec, "sta_static_dns1", cfg.sta_static_dns1);
        cJSON_AddStringToObject(sec, "sta_static_dns2", cfg.sta_static_dns2);
        {
            bool sta_connected;
            bool sta_has_ip;
            bool fallback;
            char sta_ip[16];
            char last_disconnect[32];

            app_wifi_runtime_lock();
            sta_connected = s_wifi.sta_connected;
            sta_has_ip = s_wifi.sta_has_ip;
            fallback = s_wifi.startup_fallback_to_ap;
            memcpy(sta_ip, s_wifi.sta_ip, sizeof(sta_ip));
            memcpy(last_disconnect, s_wifi.last_disconnect, sizeof(last_disconnect));
            app_wifi_runtime_unlock();
            cJSON_AddBoolToObject(sec, "sta_connected", sta_connected);
            cJSON_AddBoolToObject(sec, "sta_has_ip", sta_has_ip);
            cJSON_AddStringToObject(sec, "sta_ip", sta_ip);
            cJSON_AddStringToObject(sec, "last_disconnect", last_disconnect);
            cJSON_AddBoolToObject(sec, "startup_fallback_to_ap", fallback);
        }
    }

    sec = cJSON_AddObjectToObject(root, "mqtt");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "mqtt_backend", cfg.mqtt_backend);
        cJSON_AddStringToObject(sec, "mqtt_host", cfg.mqtt_host);
        cJSON_AddNumberToObject(sec, "mqtt_port", (double)cfg.mqtt_port);
        cJSON_AddBoolToObject(sec, "mqtt_use_tls", cfg.mqtt_use_tls != 0);
        cJSON_AddBoolToObject(sec, "mqtt_token_set", cfg.mqtt_token[0] != '\0');
        {
            bool connected;
            uint32_t publish_count;
            char last_error[sizeof(s_mqtt.last_error)];

            app_mqtt_lock();
            connected = s_mqtt.connected;
            publish_count = s_mqtt.publish_count;
            memcpy(last_error, s_mqtt.last_error, sizeof(last_error));
            app_mqtt_unlock();
            cJSON_AddBoolToObject(sec, "connected", connected);
            cJSON_AddNumberToObject(sec, "publish_count", (double)publish_count);
            cJSON_AddNumberToObject(sec, "outbox", (double)app_mqtt_outbox_size());
            cJSON_AddStringToObject(sec, "last_error", last_error);
        }
    }

    sec = cJSON_AddObjectToObject(root, "bluetooth");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "bt_role", app_bt_mode_to_string(cfg.bt_role));
        cJSON_AddStringToObject(sec, "bt_device_name", cfg.bt_device_name);
        {
            uint8_t runtime_mode = cfg.bt_role;
            char last_error[96] = {0};
            bool connected;

            app_bt_get_status(&runtime_mode, last_error, sizeof(last_error));
            connected = app_ble_gatts_connected() || app_ble_gattc_connected();
            cJSON_AddStringToObject(sec, "runtime_mode", app_bt_mode_to_string(runtime_mode));
            cJSON_AddBoolToObject(sec, "adv_active", app_bt_advertising());
            cJSON_AddBoolToObject(sec, "connected", connected);
            cJSON_AddStringToObject(sec, "last_error", last_error);
        }
    }

    sec = cJSON_AddObjectToObject(root, "uart");
    if (sec != NULL) {
        parity = (cfg.uart_parity_mode == UART_PARITY_EVEN) ? "even"
               : (cfg.uart_parity_mode == UART_PARITY_ODD) ? "odd"
               : "none";
        cJSON_AddNumberToObject(sec, "uart_baudrate", (double)cfg.uart_baudrate);
        cJSON_AddNumberToObject(sec, "uart_data_bits", (double)cfg.uart_data_bits);
        cJSON_AddNumberToObject(sec, "uart_stop_bits", (double)cfg.uart_stop_bits);
        cJSON_AddStringToObject(sec, "uart_parity_mode", parity);
        cJSON_AddNumberToObject(sec, "uart_rx_gpio", (double)app_uart_rx_gpio());
        cJSON_AddNumberToObject(sec, "uart_tx_gpio", (double)app_uart_tx_gpio());
    }

    sec = cJSON_AddObjectToObject(root, "gpio");
    if (sec != NULL) {
        app_io_state_t io;

        app_io_get_state(&io);
        cJSON_AddBoolToObject(sec, "relay_on", io.relay_on);
        cJSON_AddBoolToObject(sec, "led_on", io.led_on);
        cJSON_AddNumberToObject(sec, "relay_power_policy", cfg.relay_power_policy);
        cJSON_AddNumberToObject(sec, "led_power_policy", cfg.led_power_policy);
        cJSON_AddBoolToObject(sec, "relay_active_high", cfg.relay_active_high != 0);
        cJSON_AddBoolToObject(sec, "led_active_high", cfg.led_active_high != 0);
        cJSON_AddBoolToObject(sec, "input_active_low", cfg.input_active_low != 0);
        cJSON_AddBoolToObject(sec, "input_active", io.input_active);
        cJSON_AddNumberToObject(sec, "io_seq", io.seq);
        cJSON_AddNumberToObject(sec, "relay_gpio", (double)app_relay_gpio());
        cJSON_AddNumberToObject(sec, "led_gpio", (double)app_led_gpio());
        cJSON_AddNumberToObject(sec, "input_gpio", (double)app_input_gpio());
    }

    sec = cJSON_AddObjectToObject(root, "web");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "username", cfg.web_username);
    }

    if (domains != NULL) {
        size_t i;

        for (i = 0; i < sizeof(k_config_domains) / sizeof(k_config_domains[0]); i++) {
            if (!app_cmd_domain_wanted(domains, k_config_domains[i])) {
                cJSON_DeleteItemFromObjectCaseSensitive(root, k_config_domains[i]);
            }
        }
    }

    return root;
}

/* v1.0 探测入口：无副作用，不暴露凭据；security 事实先如实报告 false。 */
static cJSON *app_cmd_build_device_info(void)
{
    const esp_app_desc_t *desc = esp_app_get_description();
    cJSON *root = cJSON_CreateObject();
    cJSON *versions;
    cJSON *security;

    if (root == NULL) {
        return NULL;
    }
    cJSON_AddStringToObject(root, "model", desc != NULL ? desc->project_name : "iothub");
    cJSON_AddStringToObject(root, "firmware", desc != NULL ? desc->version : "");
    cJSON_AddStringToObject(root, "device_id", app_get_hostname());
    versions = cJSON_AddArrayToObject(root, "protocol_versions");
    if (versions != NULL) {
        cJSON_AddItemToArray(versions, cJSON_CreateString("1.0"));
    }
    security = cJSON_AddObjectToObject(root, "security");
    if (security != NULL) {
        cJSON_AddBoolToObject(security, "link_encrypted", false);
        cJSON_AddBoolToObject(security, "bonded", false);
        cJSON_AddBoolToObject(security, "l2_ready", false);
        cJSON_AddBoolToObject(security, "sensitive_writes_allowed", false);
    }
    return root;
}

/* 普通结果(含 success 外壳):changed 移交所有权 */
static cJSON *app_cmd_build_success(cJSON *changed, cfg_effect_t effect)
{
    cJSON *resp = cJSON_CreateObject();
    bool relay_on;
    bool led_on;

    if (resp == NULL) {
        if (changed != NULL) {
            cJSON_Delete(changed);
        }
        return NULL;
    }
    cJSON_AddBoolToObject(resp, "success", true);
    if (changed != NULL) {
        cJSON_AddItemToObject(resp, "changed", changed);
    } else {
        cJSON_AddItemToObject(resp, "changed", cJSON_CreateArray());
    }
    cJSON_AddStringToObject(resp, "effect",
                            effect == CFG_EFFECT_REBOOT ? "reboot"
                          : effect == CFG_EFFECT_SUBSYS ? "subsys"
                          : "none");
    {
        app_io_state_t io;

        app_io_get_state(&io);
        relay_on = io.relay_on;
        led_on = io.led_on;
        cJSON_AddBoolToObject(resp, "relay_on", relay_on);
        cJSON_AddBoolToObject(resp, "led_on", led_on);
        cJSON_AddBoolToObject(resp, "input_active", io.input_active);
        cJSON_AddNumberToObject(resp, "io_seq", io.seq);
        cJSON_AddStringToObject(resp, "io_source", app_io_source_name(io.last_source));
    }
    return resp;
}

/* config.set 的统一事务字段;applied 只表示运行态已接受配置,不表示服务健康 */
static void app_cmd_add_txn_fields(cJSON *resp, cfg_effect_t effect)
{
    if (resp == NULL) {
        return;
    }
    cJSON_AddBoolToObject(resp, "saved", true);
    cJSON_AddBoolToObject(resp, "applied", effect != CFG_EFFECT_REBOOT);
    cJSON_AddBoolToObject(resp, "restart_required", effect == CFG_EFFECT_REBOOT);
    cJSON_AddStringToObject(resp, "rollback", "none");
    if (effect == CFG_EFFECT_REBOOT) {
        cJSON_AddStringToObject(resp, "operation_type", "reboot");
    }
}

/* patch 执行(统一层):err_out 置静态错误串或 NULL;changed/effect 输出 */
static bool app_cmd_run_patch(const cJSON *patch, cJSON **changed_out, cfg_effect_t *effect_out,
                              const char **err_out)
{
    cJSON *changed = NULL;
    cfg_effect_t effect = CFG_EFFECT_NONE;
    esp_err_t err = config_apply_patch(patch, &changed, &effect);

    if (err == ESP_OK) {
        *changed_out = changed;
        *effect_out = effect;
        *err_out = NULL;
        return true;
    }
    if (changed != NULL) {
        cJSON_Delete(changed);
    }
    *changed_out = NULL;
    if (err == ESP_ERR_NOT_SUPPORTED) {
        *err_out = "domain_not_supported";
    } else if (err == ESP_ERR_INVALID_ARG) {
        *err_out = "invalid_config";
    } else {
        *err_out = "save_failed";
    }
    return false;
}

/* 与 Web network PUT 一致:body 中显式给出非空 sta_ssid 时,把生效后的
 * SSID/口令固化为一个已保存 profile。config_apply 只负责主配置。 */
static void app_cmd_save_profile_from_params(const cJSON *params)
{
    const cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(params, "sta_ssid");
    esp_err_t err;

    if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
        ssid_item->valuestring[0] == '\0') {
        return;
    }
    app_config_lock();
    err = app_config_save_wifi_profile(s_config.sta_ssid, s_config.sta_password);
    app_config_unlock();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "wifi profile save failed: %s", esp_err_to_name(err));
    }
}

static app_io_source_t app_cmd_io_source(app_cmd_source_t source)
{
    switch (source) {
    case APP_CMD_SOURCE_MQTT:
        return APP_IO_SOURCE_MQTT;
    case APP_CMD_SOURCE_BLE:
        return APP_IO_SOURCE_BLE;
    case APP_CMD_SOURCE_HTTP:
        return APP_IO_SOURCE_HTTP;
    case APP_CMD_SOURCE_TOUCH:
    default:
        return APP_IO_SOURCE_INPUT;
    }
}

esp_err_t app_cmd_execute_ex(const cJSON *frame, app_cmd_source_t source,
                             cJSON **resp_out, app_cmd_result_t *out,
                             const char **err_msg)
{
    app_io_source_t io_source = app_cmd_io_source(source);
    cJSON *method = NULL;
    cJSON *params = NULL;
    const char *err = NULL;
    cJSON *resp = NULL;
    bool reboot_after_response = false;
    bool changed_any = false;
    bool handled = false;
    cfg_effect_t effect = CFG_EFFECT_NONE;
    cJSON *changed_list = NULL;

    if (resp_out != NULL) {
        *resp_out = NULL;
    }
    if (err_msg != NULL) {
        *err_msg = NULL;
    }
    if (out != NULL) {
        memset(out, 0, sizeof(*out));
    }
    if (!cJSON_IsObject(frame)) {
        err = "invalid_json";
        goto done;
    }

    method = cJSON_GetObjectItemCaseSensitive(frame, "method");
    params = cJSON_GetObjectItemCaseSensitive(frame, "params");

    if (cJSON_IsString(method)) {
        const char *m = method->valuestring;

        handled = true;
        if (strcmp(m, "setConfig") == 0 || strcmp(m, "config.set") == 0) {
            if (!cJSON_IsObject(params)) {
                err = "params_required";
            } else if (!app_cmd_patch_allowed(source, params)) {
                err = "guard_rejected";
            } else if (app_cmd_run_patch(params, &changed_list, &effect, &err)) {
                changed_any = (changed_list != NULL && cJSON_GetArraySize(changed_list) > 0);
                app_cmd_save_profile_from_params(params);
                if (changed_any && effect == CFG_EFFECT_REBOOT) {
                    reboot_after_response = true;
                }
                resp = app_cmd_build_success(changed_list, effect);
                changed_list = NULL;   /* 所有权移交 resp */
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    app_cmd_add_txn_fields(resp, effect);
                }
            }
        } else if (strcmp(m, "device.info") == 0) {
            resp = app_cmd_build_device_info();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "getConfig") == 0 || strcmp(m, "config.get") == 0) {
            const cJSON *domains = cJSON_IsObject(params)
                                       ? cJSON_GetObjectItemCaseSensitive(params, "domains")
                                       : NULL;

            if (domains != NULL) {
                const cJSON *item = NULL;

                if (!cJSON_IsArray(domains)) {
                    err = "invalid_config";
                } else {
                    cJSON_ArrayForEach(item, domains) {
                        if (!cJSON_IsString(item) || item->valuestring == NULL) {
                            err = "invalid_config";
                            break;
                        }
                        if (!app_cmd_domain_known(item->valuestring)) {
                            err = "domain_not_supported";
                            break;
                        }
                    }
                }
            }
            if (err == NULL) {
                resp = app_cmd_build_get_config(domains);
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
        } else if (strcmp(m, "getStatus") == 0 || strcmp(m, "io.get") == 0) {
            uint8_t net_mode;
            app_io_state_t io;

            resp = cJSON_CreateObject();
            if (resp == NULL) {
                err = "out_of_memory";
            } else {
                app_config_lock();
                net_mode = s_config.net_mode;
                app_config_unlock();
                app_io_get_state(&io);
                cJSON_AddBoolToObject(resp, "relay_on", io.relay_on);
                cJSON_AddBoolToObject(resp, "led_on", io.led_on);
                cJSON_AddBoolToObject(resp, "input_active", io.input_active);
                cJSON_AddNumberToObject(resp, "io_seq", io.seq);
                cJSON_AddStringToObject(resp, "io_source", app_io_source_name(io.last_source));
                cJSON_AddStringToObject(resp, "net_mode", app_network_mode_to_string(net_mode));
                cJSON_AddBoolToObject(resp, "sta_has_ip", s_wifi.sta_has_ip);
                cJSON_AddStringToObject(resp, "sta_ip", s_wifi.sta_ip);
            }
        } else if (strcmp(m, "led.toggle") == 0 || strcmp(m, "toggleLed") == 0 ||
                   strcmp(m, "relay.toggle") == 0 || strcmp(m, "toggleRelay") == 0) {
            bool is_relay = (strcmp(m, "relay.toggle") == 0 || strcmp(m, "toggleRelay") == 0);
            app_io_state_t before;
            app_io_state_t after;
            bool want;
            cJSON *changed = cJSON_CreateArray();

            app_io_get_state(&before);
            want = is_relay ? !before.relay_on : !before.led_on;
            if (changed == NULL) {
                err = "out_of_memory";
            } else if (app_io_set_outputs(is_relay, want, !is_relay, want,
                                          io_source) != ESP_OK) {
                cJSON_Delete(changed);
                err = "gpio_apply_failed";
            } else {
                app_io_get_state(&after);
                changed_any = is_relay ? (before.relay_on != after.relay_on)
                                       : (before.led_on != after.led_on);
                if (changed_any) {
                    cJSON_AddItemToArray(changed, cJSON_CreateString(is_relay ? "relay_on" : "led_on"));
                }
                resp = app_cmd_build_success(changed, CFG_EFFECT_NONE);
                changed = NULL;
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
            if (changed != NULL) {
                cJSON_Delete(changed);
            }
        } else if (strcmp(m, "setRelay") == 0 || strcmp(m, "relay.set") == 0 ||
                   strcmp(m, "setLed") == 0 || strcmp(m, "led.set") == 0) {
            bool is_relay = (strcmp(m, "setRelay") == 0 || strcmp(m, "relay.set") == 0);
            app_io_state_t before;
            app_io_state_t after;
            bool want;
            cJSON *changed = cJSON_CreateArray();

            app_io_get_state(&before);
            want = app_cmd_parse_bool_param(params, !(is_relay ? before.relay_on : before.led_on));
            if (changed == NULL) {
                err = "out_of_memory";
            } else if (app_io_set_outputs(is_relay, want, !is_relay, want,
                                          io_source) != ESP_OK) {
                cJSON_Delete(changed);
                err = "gpio_apply_failed";
            } else {
                app_io_get_state(&after);
                changed_any = is_relay ? (before.relay_on != after.relay_on)
                                       : (before.led_on != after.led_on);
                if (changed_any) {
                    cJSON_AddItemToArray(changed, cJSON_CreateString(is_relay ? "relay_on" : "led_on"));
                }
                resp = app_cmd_build_success(changed, CFG_EFFECT_NONE);
                changed = NULL;
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
            if (changed != NULL) {
                cJSON_Delete(changed);
            }
        } else if (strcmp(m, "setState") == 0 || strcmp(m, "io.set") == 0) {
            cJSON *rel = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "relay_on") : NULL;
            cJSON *led = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "led_on") : NULL;
            bool set_relay = cJSON_IsBool(rel) || cJSON_IsNumber(rel);
            bool set_led = cJSON_IsBool(led) || cJSON_IsNumber(led);
            bool relay_on = set_relay ? (cJSON_IsBool(rel) ? cJSON_IsTrue(rel) : (rel->valueint != 0)) : false;
            bool led_on = set_led ? (cJSON_IsBool(led) ? cJSON_IsTrue(led) : (led->valueint != 0)) : false;
            app_io_state_t before;
            app_io_state_t after;
            cJSON *changed = NULL;

            if (!set_relay && !set_led) {
                err = "params_required";
            } else {
                app_io_get_state(&before);
                changed = cJSON_CreateArray();
                if (changed == NULL) {
                    err = "out_of_memory";
                } else if (app_io_set_outputs(set_relay, relay_on, set_led, led_on,
                                              io_source) != ESP_OK) {
                    cJSON_Delete(changed);
                    changed = NULL;
                    err = "gpio_apply_failed";
                } else {
                    app_io_get_state(&after);
                    changed_any = (before.relay_on != after.relay_on) ||
                                  (before.led_on != after.led_on);
                    if (before.relay_on != after.relay_on) {
                        cJSON_AddItemToArray(changed, cJSON_CreateString("relay_on"));
                    }
                    if (before.led_on != after.led_on) {
                        cJSON_AddItemToArray(changed, cJSON_CreateString("led_on"));
                    }
                    resp = app_cmd_build_success(changed, CFG_EFFECT_NONE);
                    changed = NULL;
                    if (resp == NULL) {
                        err = "out_of_memory";
                    }
                }
                if (changed != NULL) {
                    cJSON_Delete(changed);
                }
            }
        } else if (strcmp(m, "web.auth.set") == 0) {
            const cJSON *old_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "old_password")
                                        : NULL;
            const cJSON *new_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "new_password")
                                        : NULL;
            const cJSON *user_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "new_username")
                                         : NULL;
            const char *new_user = cJSON_IsString(user_item) ? user_item->valuestring : NULL;
            uint8_t salt[16];

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(old_item) || old_item->valuestring == NULL ||
                       !app_cmd_verify_web_password(old_item->valuestring)) {
                err = "invalid_old_password";
            } else if (!cJSON_IsString(new_item) || new_item->valuestring == NULL ||
                       (strlen(new_item->valuestring) < 8 &&
                        strcmp(new_item->valuestring, CONFIG_WEB_DEFAULT_PASSWORD) != 0)) {
                err = "password_too_short";
            } else if (new_user != NULL && new_user[0] != '\0' &&
                       strchr(new_user, ':') != NULL) {
                err = "invalid_username";
            } else {
                esp_err_t save_err;

                esp_fill_random(salt, sizeof(salt));
                app_config_lock();
                if (new_user != NULL && new_user[0] != '\0') {
                    app_copy_string(s_config.web_username, sizeof(s_config.web_username), new_user);
                }
                memcpy(s_config.web_pass_salt, salt, sizeof(salt));
                app_web_hash_password(salt, sizeof(salt), new_item->valuestring,
                                      s_config.web_pass_hash);
                save_err = app_config_save(&s_config);
                app_config_unlock();
                if (save_err != ESP_OK) {
                    err = "save_failed";
                } else {
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddBoolToObject(resp, "saved", true);
                    }
                }
            }
        } else if (strcmp(m, "provisioning.pin.set") == 0) {
            const cJSON *pin_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "pin")
                                        : NULL;
            const cJSON *pass_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "password")
                                         : NULL;
            esp_err_t save_err;

            /* 配网 PIN 属于敏感操作：只允许本地 HTTP/BLE，且要求当前管理员口令。
             * 不回显 PIN，不写日志；新值在下次进入配网时生效。 */
            if (source != APP_CMD_SOURCE_HTTP && source != APP_CMD_SOURCE_BLE) {
                err = "forbidden";
            } else if (!cJSON_IsString(pin_item) || pin_item->valuestring == NULL ||
                       !app_prov_pin_is_valid(pin_item->valuestring)) {
                err = "invalid_param";
            } else if (!cJSON_IsString(pass_item) || pass_item->valuestring == NULL ||
                       !app_cmd_verify_web_password(pass_item->valuestring)) {
                err = "unauthorized";
            } else {
                save_err = app_prov_pin_set(pin_item->valuestring);
                if (save_err != ESP_OK) {
                    err = "save_failed";
                } else {
                    cJSON *changed;

                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddBoolToObject(resp, "saved", true);
                        cJSON_AddBoolToObject(resp, "applied", true);
                        cJSON_AddBoolToObject(resp, "restart_required", false);
                        cJSON_AddStringToObject(resp, "effect", "none");
                        cJSON_AddBoolToObject(resp, "pin_set", true);
                        changed = cJSON_AddArrayToObject(resp, "changed");
                        if (changed != NULL) {
                            cJSON_AddItemToArray(changed,
                                                 cJSON_CreateString("provisioning_pin"));
                        }
                    }
                }
            }
        } else if (strcmp(m, "uart.send") == 0) {
            const cJSON *enc_item = cJSON_IsObject(params)
                                        ? cJSON_GetObjectItemCaseSensitive(params, "encoding")
                                        : NULL;
            const cJSON *data_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "data")
                                         : NULL;
            const char *encoding = cJSON_IsString(enc_item) ? enc_item->valuestring : "text";
            unsigned sent = 0;
            esp_err_t uart_err;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(data_item) || data_item->valuestring == NULL) {
                err = "params_required";
            } else {
                uart_err = app_uart_send_data(encoding, data_item->valuestring, &sent);
                if (uart_err != ESP_OK) {
                    err = "uart_send_failed";
                } else {
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddNumberToObject(resp, "sent", (double)sent);
                    }
                }
            }
        } else if (strcmp(m, "data.loopback") == 0) {
            const cJSON *enabled_item = cJSON_IsObject(params)
                                            ? cJSON_GetObjectItemCaseSensitive(params, "enabled")
                                            : NULL;
            bool enabled;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsBool(enabled_item) && !cJSON_IsNumber(enabled_item)) {
                err = "params_required";
            } else {
                enabled = cJSON_IsBool(enabled_item) ? cJSON_IsTrue(enabled_item)
                                                      : (enabled_item->valueint != 0);
                data_pipe_set_loopback(enabled);
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddBoolToObject(resp, "loopback", enabled);
                }
            }
        } else if (strcmp(m, "network.status") == 0) {
            resp = app_cmd_build_network_status();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "network.tryconnect") == 0) {
            const cJSON *ssid_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "ssid")
                                         : NULL;
            const cJSON *pass_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "password")
                                         : NULL;
            app_wifi_try_result_t result;
            const char *ssid;
            const char *password;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
                       ssid_item->valuestring[0] == '\0') {
                err = "params_required";
            } else if (s_net_trial.valid && app_cmd_net_trial_ready(NULL)) {
                err = "busy";
            } else {
                app_cmd_net_trial_clear();
                ssid = ssid_item->valuestring;
                password = cJSON_IsString(pass_item) && pass_item->valuestring != NULL
                               ? pass_item->valuestring : "";
                ESP_LOGI(TAG, "network.tryconnect ssid='%s'", ssid);
                result = app_wifi_try_connect_ip(ssid, password, 15000);
                switch (result) {
                case APP_WIFI_TRY_OK:
                    snprintf(s_net_trial.id, sizeof(s_net_trial.id), "trial-%08X",
                             (unsigned)esp_random());
                    snprintf(s_net_trial.ssid, sizeof(s_net_trial.ssid), "%s", ssid);
                    snprintf(s_net_trial.password, sizeof(s_net_trial.password), "%s", password);
                    s_net_trial.expires_ms = esp_timer_get_time() / 1000 + APP_NET_TRIAL_TTL_MS;
                    s_net_trial.valid = true;
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        app_cmd_net_trial_clear();
                        err = "out_of_memory";
                    } else {
                        cJSON_AddStringToObject(resp, "trial_id", s_net_trial.id);
                        cJSON_AddStringToObject(resp, "state", "got_ip");
                        cJSON_AddStringToObject(resp, "ssid", s_net_trial.ssid);
                    }
                    break;
                case APP_WIFI_TRY_WRONG_PASSWORD:
                    err = "wrong_password";
                    break;
                case APP_WIFI_TRY_AP_NOT_FOUND:
                    err = "ap_not_found";
                    break;
                case APP_WIFI_TRY_TIMEOUT:
                    err = "connect_timeout";
                    break;
                case APP_WIFI_TRY_BUSY:
                    err = "busy";
                    break;
                case APP_WIFI_TRY_INVALID_ARG:
                    err = "invalid_param";
                    break;
                default:
                    err = "connect_failed";
                    break;
                }
            }
        } else if (strcmp(m, "network.apply") == 0) {
            const cJSON *trial_item = cJSON_IsObject(params)
                                          ? cJSON_GetObjectItemCaseSensitive(params, "trial_id")
                                          : NULL;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(trial_item) || trial_item->valuestring == NULL ||
                       !app_cmd_net_trial_ready(trial_item->valuestring)) {
                err = "trial_invalid";
            } else if (app_config_snapshot_save() != ESP_OK) {
                app_cmd_net_trial_clear();
                err = "storage_failed";
            } else {
                cJSON *patch = cJSON_CreateObject();
                cJSON *changed = NULL;
                cfg_effect_t effect = CFG_EFFECT_NONE;
                esp_err_t apply_err = ESP_FAIL;

                if (patch != NULL) {
                    cJSON_AddStringToObject(patch, "sta_ssid", s_net_trial.ssid);
                    cJSON_AddStringToObject(patch, "sta_password", s_net_trial.password);
                    apply_err = config_apply_patch(patch, &changed, &effect);
                    cJSON_Delete(patch);
                    cJSON_Delete(changed);
                }
                if (apply_err != ESP_OK) {
                    app_config_snapshot_discard();
                    app_cmd_net_trial_clear();
                    err = apply_err == ESP_ERR_INVALID_ARG ? "invalid_config" : "save_failed";
                } else {
                    esp_err_t profile_err = app_config_save_wifi_profile(s_net_trial.ssid,
                                                                          s_net_trial.password);

                    if (profile_err != ESP_OK) {
                        esp_err_t restore_err = app_config_snapshot_restore();

                        app_cmd_net_trial_clear();
                        err = restore_err == ESP_OK ? "storage_failed" : "rollback_failed";
                    } else if (!app_wifi_schedule_restart()) {
                        esp_err_t restore_err = app_config_snapshot_restore();

                        app_cmd_net_trial_clear();
                        err = restore_err == ESP_OK ? "apply_failed" : "rollback_failed";
                    } else {
                        char trial_id[sizeof(s_net_trial.id)];

                        snprintf(trial_id, sizeof(trial_id), "%s", s_net_trial.id);
                        app_config_snapshot_discard();
                        app_cmd_net_trial_clear();
                        resp = cJSON_CreateObject();
                        if (resp == NULL) {
                            err = "out_of_memory";
                        } else {
                            cJSON_AddStringToObject(resp, "trial_id", trial_id);
                            cJSON_AddStringToObject(resp, "state", "success");
                            cJSON_AddBoolToObject(resp, "saved", true);
                            cJSON_AddBoolToObject(resp, "applied", true);
                        }
                    }
                }
            }
        } else if (strcmp(m, "network.cancel") == 0) {
            const cJSON *trial_item = cJSON_IsObject(params)
                                          ? cJSON_GetObjectItemCaseSensitive(params, "trial_id")
                                          : NULL;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(trial_item) || trial_item->valuestring == NULL ||
                       !app_cmd_net_trial_ready(trial_item->valuestring)) {
                err = "not_found";
            } else {
                char trial_id[sizeof(s_net_trial.id)];

                snprintf(trial_id, sizeof(trial_id), "%s", s_net_trial.id);
                app_cmd_net_trial_clear();
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddStringToObject(resp, "trial_id", trial_id);
                    cJSON_AddStringToObject(resp, "state", "cancelled");
                }
            }
        } else if (strcmp(m, "network.profiles") == 0) {
            app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX] = {0};
            size_t profile_count = 0;
            cJSON *arr;

            if (app_config_load_wifi_profiles(profiles, APP_WIFI_PROFILE_MAX,
                                              &profile_count) != ESP_OK) {
                profile_count = 0;
            }
            resp = cJSON_CreateObject();
            if (resp == NULL) {
                err = "out_of_memory";
            } else {
                arr = cJSON_AddArrayToObject(resp, "profiles");
                if (arr != NULL) {
                    size_t i;

                    for (i = 0; i < profile_count; i++) {
                        cJSON *item = cJSON_CreateObject();

                        if (item == NULL) {
                            continue;
                        }
                        cJSON_AddStringToObject(item, "ssid", profiles[i].ssid);
                        cJSON_AddBoolToObject(item, "password_set",
                                              profiles[i].password[0] != '\0');
                        cJSON_AddItemToArray(arr, item);
                    }
                }
            }
        } else if (strcmp(m, "network.scan") == 0) {
            cJSON *arr;
            uint16_t scan_count;
            uint16_t index;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (app_wifi_perform_scan() != ESP_OK) {
                err = "wifi_scan_failed";
            } else {
                app_wifi_runtime_lock();
                scan_count = s_wifi.scan_count;
                app_wifi_runtime_unlock();
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddStringToObject(resp, "status", "ok");
                    cJSON_AddNumberToObject(resp, "count", (double)scan_count);
                    arr = cJSON_AddArrayToObject(resp, "aps");
                    if (arr != NULL) {
                        for (index = 0; index < scan_count; index++) {
                            cJSON *ap = cJSON_CreateObject();
                            const char *ssid = "";
                            int8_t rssi = 0;
                            uint8_t authmode = 0;
                            char bssid[18];

                            if (ap == NULL) {
                                continue;
                            }
                            if (!app_wifi_scan_record_at(index, &ssid, &rssi, &authmode)) {
                                cJSON_Delete(ap);
                                continue;
                            }
                            cJSON_AddStringToObject(ap, "ssid", ssid);
                            cJSON_AddNumberToObject(ap, "rssi", (double)rssi);
                            cJSON_AddNumberToObject(ap, "auth", (double)authmode);
                            if (app_wifi_scan_record_bssid(index, bssid, sizeof(bssid))) {
                                cJSON_AddStringToObject(ap, "bssid", bssid);
                            }
                            cJSON_AddItemToArray(arr, ap);
                        }
                    }
                }
            }
        } else if (strcmp(m, "network.profile.delete") == 0) {
            const cJSON *ssid_item = cJSON_IsObject(params)
                                         ? cJSON_GetObjectItemCaseSensitive(params, "ssid")
                                         : NULL;

            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!cJSON_IsString(ssid_item) || ssid_item->valuestring == NULL ||
                       ssid_item->valuestring[0] == '\0') {
                err = "params_required";
            } else {
                esp_err_t del_err = app_config_delete_wifi_profile(ssid_item->valuestring);

                if (del_err == ESP_ERR_NOT_FOUND) {
                    err = "not_found";
                } else if (del_err != ESP_OK) {
                    err = "delete_failed";
                } else {
                    resp = cJSON_CreateObject();
                    if (resp == NULL) {
                        err = "out_of_memory";
                    } else {
                        cJSON_AddStringToObject(resp, "deleted", ssid_item->valuestring);
                    }
                }
            }
        } else if (strcmp(m, "network.restart") == 0) {
            if (source == APP_CMD_SOURCE_MQTT) {
                err = "guard_rejected";
            } else if (!app_wifi_schedule_restart()) {
                err = "restart_failed";
            } else {
                resp = cJSON_CreateObject();
                if (resp == NULL) {
                    err = "out_of_memory";
                } else {
                    cJSON_AddBoolToObject(resp, "restarting", true);
                }
            }
        } else {
            err = "unknown_method";
        }
    }

done:
    if (changed_list != NULL) {
        cJSON_Delete(changed_list);
    }
    if (err != NULL && resp != NULL) {
        cJSON_Delete(resp);
        resp = NULL;
    }
    if (resp_out != NULL) {
        *resp_out = resp;
    } else if (resp != NULL) {
        cJSON_Delete(resp);
    }
    if (out != NULL) {
        out->reboot_after_response = reboot_after_response;
        out->changed_any = changed_any;
        out->handled = handled;
    }
    if (err_msg != NULL) {
        *err_msg = err;
    }
    return err != NULL ? ESP_FAIL : ESP_OK;
}
