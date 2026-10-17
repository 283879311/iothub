#include <stdbool.h>
#include <string.h>

#include "cJSON.h"
#include "esp_log.h"

#include "cmd.h"
#include "common.h"
#include "config.h"
#include "config_apply.h"
#include "constants.h"
#include "state.h"
#include "wifi.h"

static const char *TAG = "cmd";

extern app_config_t s_config;

/* ---------------------------------------------------------------------------
 * 护栏:云端/BLE 命令面可改网络参数的开关 —— 默认关闭(改错会自断通道,只能现场恢复)。
 * 开启需改此宏重编(不做远端/网页开关,防止"云端自己开自己")。1.3-31/1.3-32 共用。
 * ------------------------------------------------------------------------- */
#define APP_CLOUD_ALLOW_NET_CMDS 0

static bool app_cmd_cloud_patch_allowed(const cJSON *params)
{
#if APP_CLOUD_ALLOW_NET_CMDS
    (void)params;
    return true;
#else
    const cJSON *item = NULL;

    if (!cJSON_IsObject(params)) {
        return true;   /* 非法 params 由统一层报错 */
    }
    cJSON_ArrayForEach(item, params) {
        const cfg_field_t *f = config_apply_field(item->string != NULL ? item->string : "");

        if (f != NULL && (f->domain == CFG_DOMAIN_NET || f->domain == CFG_DOMAIN_MQTT)) {
            ESP_LOGW(TAG, "cloud patch rejected (guard): field '%s' in net/mqtt domain",
                     item->string);
            return false;
        }
    }
    return true;
#endif
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

/* 统一层 patch 调用前先释放上一次的 changed 列表,避免双写路径泄漏 */
static void app_cmd_patch_reset(cJSON **changed_list)
{
    if (*changed_list != NULL) {
        cJSON_Delete(*changed_list);
        *changed_list = NULL;
    }
}

/* getConfig 的脱敏汇总:与 HTTP GET 同规则 —— 口令/token 永不回明文,只回 *_set */
static cJSON *app_cmd_build_get_config(void)
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
    }

    sec = cJSON_AddObjectToObject(root, "mqtt");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "mqtt_backend", cfg.mqtt_backend);
        cJSON_AddStringToObject(sec, "mqtt_host", cfg.mqtt_host);
        cJSON_AddNumberToObject(sec, "mqtt_port", (double)cfg.mqtt_port);
        cJSON_AddBoolToObject(sec, "mqtt_use_tls", cfg.mqtt_use_tls != 0);
        cJSON_AddBoolToObject(sec, "mqtt_token_set", cfg.mqtt_token[0] != '\0');
    }

    sec = cJSON_AddObjectToObject(root, "bluetooth");
    if (sec != NULL) {
        cJSON_AddStringToObject(sec, "bt_role", app_bt_mode_to_string(cfg.bt_role));
        cJSON_AddStringToObject(sec, "bt_device_name", cfg.bt_device_name);
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
    }

    sec = cJSON_AddObjectToObject(root, "gpio");
    if (sec != NULL) {
        cJSON_AddBoolToObject(sec, "relay_on", cfg.relay_on != 0);
        cJSON_AddBoolToObject(sec, "led_on", cfg.led_on != 0);
        cJSON_AddBoolToObject(sec, "relay_active_high", cfg.relay_active_high != 0);
        cJSON_AddBoolToObject(sec, "led_active_high", cfg.led_active_high != 0);
        cJSON_AddBoolToObject(sec, "input_active_low", cfg.input_active_low != 0);
        cJSON_AddBoolToObject(sec, "input_active", app_get_input_state());
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
    app_config_lock();
    relay_on = s_config.relay_on != 0;
    led_on = s_config.led_on != 0;
    app_config_unlock();
    cJSON_AddBoolToObject(resp, "relay_on", relay_on);
    cJSON_AddBoolToObject(resp, "led_on", led_on);
    return resp;
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

esp_err_t app_cmd_execute(const cJSON *frame, cJSON **resp_out, app_cmd_result_t *out,
                          const char **err_msg)
{
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
        if (strcmp(m, "setConfig") == 0) {
            if (!cJSON_IsObject(params)) {
                err = "params_required";
            } else if (!app_cmd_cloud_patch_allowed(params)) {
                err = "guard_rejected";
            } else if (app_cmd_run_patch(params, &changed_list, &effect, &err)) {
                changed_any = (changed_list != NULL && cJSON_GetArraySize(changed_list) > 0);
                if (changed_any && effect == CFG_EFFECT_REBOOT) {
                    reboot_after_response = true;
                }
                resp = app_cmd_build_success(changed_list, effect);
                changed_list = NULL;   /* 所有权移交 resp */
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
        } else if (strcmp(m, "getConfig") == 0) {
            resp = app_cmd_build_get_config();
            if (resp == NULL) {
                err = "out_of_memory";
            }
        } else if (strcmp(m, "getStatus") == 0) {
            uint8_t net_mode;
            bool relay_on;
            bool led_on;

            resp = cJSON_CreateObject();
            if (resp == NULL) {
                err = "out_of_memory";
            } else {
                app_config_lock();
                relay_on = s_config.relay_on != 0;
                led_on = s_config.led_on != 0;
                net_mode = s_config.net_mode;
                app_config_unlock();
                cJSON_AddBoolToObject(resp, "relay_on", relay_on);
                cJSON_AddBoolToObject(resp, "led_on", led_on);
                cJSON_AddBoolToObject(resp, "input_active", app_get_input_state());
                cJSON_AddStringToObject(resp, "net_mode", app_network_mode_to_string(net_mode));
                cJSON_AddBoolToObject(resp, "sta_has_ip", s_wifi.sta_has_ip);
                cJSON_AddStringToObject(resp, "sta_ip", s_wifi.sta_ip);
            }
        } else if (strcmp(m, "setRelay") == 0 || strcmp(m, "setLed") == 0) {
            bool is_relay = (strcmp(m, "setRelay") == 0);
            bool cur;
            bool want;
            cJSON *patch = cJSON_CreateObject();

            app_config_lock();
            cur = is_relay ? s_config.relay_on : s_config.led_on;
            app_config_unlock();
            want = app_cmd_parse_bool_param(params, !cur);   /* 无值 = 取反,保持旧语义 */
            if (patch == NULL) {
                err = "out_of_memory";
            } else {
                cJSON_AddBoolToObject(patch, is_relay ? "relay_on" : "led_on", want);
                if (app_cmd_run_patch(patch, &changed_list, &effect, &err)) {
                    changed_any = (changed_list != NULL && cJSON_GetArraySize(changed_list) > 0);
                    resp = app_cmd_build_success(changed_list, effect);
                    changed_list = NULL;
                    if (resp == NULL) {
                        err = "out_of_memory";
                    }
                }
                cJSON_Delete(patch);
            }
        } else if (strcmp(m, "setState") == 0) {
            cJSON *rel = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "relay_on") : NULL;
            cJSON *led = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "led_on") : NULL;
            cJSON *patch = cJSON_CreateObject();

            if (patch == NULL) {
                err = "out_of_memory";
            } else {
                if (cJSON_IsBool(rel) || cJSON_IsNumber(rel)) {
                    cJSON_AddBoolToObject(patch, "relay_on",
                                          cJSON_IsBool(rel) ? cJSON_IsTrue(rel) : (rel->valueint != 0));
                }
                if (cJSON_IsBool(led) || cJSON_IsNumber(led)) {
                    cJSON_AddBoolToObject(patch, "led_on",
                                          cJSON_IsBool(led) ? cJSON_IsTrue(led) : (led->valueint != 0));
                }
                if (app_cmd_run_patch(patch, &changed_list, &effect, &err)) {
                    changed_any = (changed_list != NULL && cJSON_GetArraySize(changed_list) > 0);
                    resp = app_cmd_build_success(changed_list, effect);
                    changed_list = NULL;
                    if (resp == NULL) {
                        err = "out_of_memory";
                    }
                }
                cJSON_Delete(patch);
            }
        } else {
            err = "unknown_method";
        }
    }

    /* 顶层 relay_on/led_on 键(共享属性/旧写法)→ 统一层 GPIO patch */
    {
        cJSON *rel = cJSON_GetObjectItemCaseSensitive(frame, "relay_on");
        cJSON *led = cJSON_GetObjectItemCaseSensitive(frame, "led_on");
        cJSON *patch = cJSON_CreateObject();
        bool has = false;

        if (cJSON_IsBool(rel) || cJSON_IsNumber(rel)) {
            cJSON_AddBoolToObject(patch, "relay_on",
                                  cJSON_IsBool(rel) ? cJSON_IsTrue(rel) : (rel->valueint != 0));
            has = true;
        }
        if (cJSON_IsBool(led) || cJSON_IsNumber(led)) {
            cJSON_AddBoolToObject(patch, "led_on",
                                  cJSON_IsBool(led) ? cJSON_IsTrue(led) : (led->valueint != 0));
            has = true;
        }
        if (has && patch != NULL && err == NULL) {
            handled = true;
            app_cmd_patch_reset(&changed_list);
            if (app_cmd_run_patch(patch, &changed_list, &effect, &err)) {
                changed_any = (changed_list != NULL && cJSON_GetArraySize(changed_list) > 0);
                if (resp != NULL) {
                    cJSON_Delete(resp);   /* 方法结果被顶层键结果取代(旧语义:同帧二选一按序) */
                }
                resp = app_cmd_build_success(changed_list, effect);
                changed_list = NULL;
                if (resp == NULL) {
                    err = "out_of_memory";
                }
            }
        }
        if (patch != NULL) {
            cJSON_Delete(patch);
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
