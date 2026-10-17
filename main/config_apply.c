#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#include "cJSON.h"
#include "esp_log.h"
#include "lwip/ip4_addr.h"

#include "common.h"
#include "config.h"
#include "config_apply.h"
#include "data_pipe.h"
#include "mqtt.h"
#include "state.h"
#include "wifi.h"   /* app_network_mode_from_string():net_mode 的"名称→枚举"归一 */

static const char *TAG = "cfg_apply";

extern app_config_t s_config;

#define CFG_OFF(name) offsetof(app_config_t, name)
#define CFG_LEN(name) sizeof(((app_config_t *)0)->name)
/* 结构体里最长的字符串字段(mqtt_token[128])——用于栈上转换缓冲 */
#define CFG_STR_MAX 128

/* ---------------------------------------------------------------------------
 * 值转换:与既有 app_json_find_* 语义保持一致(同时接受数字与字符串写法),
 * 区别是 patch 只解析一次 body,不再为每个字段重复 cJSON_Parse。
 * ------------------------------------------------------------------------- */
static bool cfg_coerce_bool8(const cJSON *v, uint8_t *out)
{
    if (v == NULL || out == NULL) {
        return false;
    }
    if (cJSON_IsBool(v)) {
        *out = cJSON_IsTrue(v) ? 1 : 0;
        return true;
    }
    if (cJSON_IsNumber(v)) {
        *out = (cJSON_GetNumberValue(v) != 0.0) ? 1 : 0;
        return true;
    }
    if (cJSON_IsString(v)) {
        const char *s = cJSON_GetStringValue(v);
        if (s == NULL) {
            return false;
        }
        if (strcasecmp(s, "true") == 0 || strcmp(s, "1") == 0 ||
            strcasecmp(s, "yes") == 0 || strcasecmp(s, "on") == 0) {
            *out = 1;
            return true;
        }
        if (strcasecmp(s, "false") == 0 || strcmp(s, "0") == 0 ||
            strcasecmp(s, "no") == 0 || strcasecmp(s, "off") == 0) {
            *out = 0;
            return true;
        }
    }
    return false;
}

static bool cfg_coerce_uint(const cJSON *v, uint32_t max, uint32_t *out)
{
    double n;
    if (v == NULL || out == NULL) {
        return false;
    }
    if (cJSON_IsNumber(v)) {
        n = cJSON_GetNumberValue(v);
    } else if (cJSON_IsString(v)) {
        const char *s = cJSON_GetStringValue(v);
        char *end = NULL;
        if (s == NULL || *s == '\0' || *s == '-') {
            return false;
        }
        n = (double)strtoul(s, &end, 10);
        if (end == s || *end != '\0') {
            return false;
        }
    } else {
        return false;
    }
    if (n < 0.0 || n > (double)max) {
        return false;
    }
    *out = (uint32_t)n;
    return true;
}

static bool cfg_coerce_str(const cJSON *v, char *out, size_t out_size)
{
    if (v == NULL || out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';
    if (cJSON_IsString(v)) {
        const char *s = cJSON_GetStringValue(v);
        if (s == NULL) {
            return false;
        }
        app_copy_string(out, out_size, s);
        return true;
    }
    if (cJSON_IsNumber(v)) {
        int n = snprintf(out, out_size, "%g", cJSON_GetNumberValue(v));
        return (n > 0 && (size_t)n < out_size);
    }
    if (cJSON_IsBool(v)) {
        app_copy_string(out, out_size, cJSON_IsTrue(v) ? "true" : "false");
        return true;
    }
    return false;
}

/* ---------------------------------------------------------------------------
 * 校验器(可选):只有需要范围/枚举约束的字段才登记
 * ------------------------------------------------------------------------- */
static esp_err_t cfg_validate_mqtt_port(const cJSON *value, const char *field,
                                        char *err, size_t err_size)
{
    uint32_t port = 0;
    if (!cfg_coerce_uint(value, 65535, &port) || port == 0) {
        snprintf(err, err_size, "%s must be 1..65535", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* net_mode:HTTP 前端传**字符串**("ap"/"sta"/"apsta"),命令面可传数字 0/1/2 —— 两者都接受。
 * 注意:校验器在 empty_keeps 之前执行,字符串分支必须显式白名单,不能依赖
 * from_string() 的"未知回落默认值"行为(否则非法值会被静默接受)。 */
static esp_err_t cfg_validate_net_mode(const cJSON *value, const char *field,
                                       char *err, size_t err_size)
{
    uint32_t mode = 0xFFFFFFFFu;

    if (cJSON_IsString(value)) {
        const char *s = cJSON_GetStringValue(value);

        if (s == NULL || (strcasecmp(s, "ap") != 0 && strcasecmp(s, "sta") != 0 &&
                          strcasecmp(s, "apsta") != 0)) {
            snprintf(err, err_size, "%s must be ap/sta/apsta or 0/1/2", field);
            return ESP_ERR_INVALID_ARG;
        }
        return ESP_OK;
    }
    if (!cfg_coerce_uint(value, 255, &mode) ||
        (mode != NET_MODE_AP && mode != NET_MODE_STA && mode != NET_MODE_APSTA)) {
        snprintf(err, err_size, "%s must be ap/sta/apsta or 0/1/2", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* net_mode 赋值:统一层类型是 U8,但前端惯例是字符串 → 归一在这里完成 */
static bool cfg_special_net_mode(app_config_t *dst, const cJSON *value)
{
    uint8_t mode;

    if (cJSON_IsString(value)) {
        const char *s = cJSON_GetStringValue(value);
        mode = app_network_mode_from_string(s != NULL ? s : "");
    } else {
        mode = (uint8_t)cJSON_GetNumberValue(value);
    }
    if (mode == dst->net_mode) {
        return false;   /* 无变化 */
    }
    dst->net_mode = mode;
    return true;
}

/* net 域一致性推断(1.3-04 实测缺陷):设备处于"纯 AP"时,用户在门户里只填了上游 SSID
 * (模式下拉仍是默认 AP)→ SSID/口令全部落盘,但 STA 永不启动、也没有任何报错,
 * 现象就是"填了 SSID 拿不到 IP"。
 * 规则:本次 patch 触及 net 域 且 结果态 sta_ssid 非空 且 模式不含 STA
 *       → 自动升级为 AP+STA(保留 AP,便于继续从 AP 侧访问/回退)。
 * 想显式保持纯 AP:提交 mode="ap" + sta_ssid_clear=true。 */
static bool cfg_net_infer_sta_mode(app_config_t *cfg)
{
    if (cfg->sta_ssid[0] == '\0' || app_network_mode_has_sta(cfg->net_mode)) {
        return false;
    }
    cfg->net_mode = app_network_mode_has_ap(cfg->net_mode) ? NET_MODE_APSTA : NET_MODE_STA;
    return true;
}

/* 网络口令:空串 = 保持(由 empty_keeps 短路),非空时要求 >= 8(与既有 handler 一致)。
 * 注意:校验器在 empty_keeps 判定**之前**执行,故必须接受空串。 */
static esp_err_t cfg_validate_wifi_password(const cJSON *value, const char *field,
                                            char *err, size_t err_size)
{
    char tmp[CFG_STR_MAX + 1];

    if (!cfg_coerce_str(value, tmp, sizeof(tmp))) {
        snprintf(err, err_size, "%s must be string", field);
        return ESP_ERR_INVALID_ARG;
    }
    if (tmp[0] != '\0' && strlen(tmp) < 8) {
        snprintf(err, err_size, "%s too short (min 8)", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* 跨字段校验:改了 SSID 却没给合法口令(空/太短)时,必须拒绝。
 * 否则用户填新 SSID 但留空/错填口令,设备会保存并重启,然后连不上该 AP。 */
static bool cfg_validate_net_cross(const app_config_t *old_cfg, const app_config_t *new_cfg,
                                   char *err, size_t err_size)
{
    if (new_cfg->ap_ssid[0] != '\0' &&
        strcmp(old_cfg->ap_ssid, new_cfg->ap_ssid) != 0 &&
        strlen(new_cfg->ap_password) < 8) {
        snprintf(err, err_size, "ap_password required when changing ap_ssid");
        return false;
    }
    if (new_cfg->sta_ssid[0] != '\0' &&
        strcmp(old_cfg->sta_ssid, new_cfg->sta_ssid) != 0 &&
        strlen(new_cfg->sta_password) < 8) {
        snprintf(err, err_size, "sta_password required when changing sta_ssid");
        return false;
    }
    return true;
}

/* IPv4 格式校验:空串视为"未提供/保持原值",仅对非空串检查合法性 */
static esp_err_t cfg_validate_ipv4(const cJSON *value, const char *field,
                                   char *err, size_t err_size)
{
    char tmp[CFG_STR_MAX + 1];
    ip4_addr_t addr;

    if (!cfg_coerce_str(value, tmp, sizeof(tmp))) {
        snprintf(err, err_size, "%s must be string", field);
        return ESP_ERR_INVALID_ARG;
    }
    if (tmp[0] == '\0') {
        return ESP_OK;
    }
    if (!ip4addr_aton(tmp, &addr)) {
        snprintf(err, err_size, "%s invalid IPv4 address", field);
        return ESP_ERR_INVALID_ARG;
    }
    if (ip4_addr_isany_val(addr)) {
        snprintf(err, err_size, "%s cannot be 0.0.0.0", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* 静态 IP 跨字段校验:启用静态 IP 时,IP/网关/掩码必须齐全且合法;
 * DNS 为可选项,若有值也必须合法。 */
static bool cfg_validate_static_ip_cross(const app_config_t *old_cfg, const app_config_t *new_cfg,
                                         char *err, size_t err_size)
{
    ip4_addr_t addr;

    (void)old_cfg;
    if (!new_cfg->sta_use_static) {
        return true;
    }
    if (new_cfg->sta_static_ip[0] == '\0') {
        snprintf(err, err_size, "sta_static_ip required when static IP enabled");
        return false;
    }
    if (new_cfg->sta_static_gateway[0] == '\0') {
        snprintf(err, err_size, "sta_static_gateway required when static IP enabled");
        return false;
    }
    if (new_cfg->sta_static_netmask[0] == '\0') {
        snprintf(err, err_size, "sta_static_netmask required when static IP enabled");
        return false;
    }
    if (!ip4addr_aton(new_cfg->sta_static_ip, &addr) || ip4_addr_isany_val(addr)) {
        snprintf(err, err_size, "sta_static_ip invalid");
        return false;
    }
    if (!ip4addr_aton(new_cfg->sta_static_gateway, &addr) || ip4_addr_isany_val(addr)) {
        snprintf(err, err_size, "sta_static_gateway invalid");
        return false;
    }
    if (!ip4addr_aton(new_cfg->sta_static_netmask, &addr) || ip4_addr_isany_val(addr)) {
        snprintf(err, err_size, "sta_static_netmask invalid");
        return false;
    }
    if (new_cfg->sta_static_dns1[0] != '\0' &&
        (!ip4addr_aton(new_cfg->sta_static_dns1, &addr) || ip4_addr_isany_val(addr))) {
        snprintf(err, err_size, "sta_static_dns1 invalid");
        return false;
    }
    if (new_cfg->sta_static_dns2[0] != '\0' &&
        (!ip4addr_aton(new_cfg->sta_static_dns2, &addr) || ip4_addr_isany_val(addr))) {
        snprintf(err, err_size, "sta_static_dns2 invalid");
        return false;
    }
    return true;
}

static esp_err_t cfg_validate_bt_role(const cJSON *value, const char *field,
                                      char *err, size_t err_size)
{
    uint32_t role = 0xFFFFFFFFu;
    if (!cfg_coerce_uint(value, 255, &role) ||
        (role != BT_ROLE_OFF && role != BT_ROLE_PERIPHERAL && role != BT_ROLE_CENTRAL)) {
        snprintf(err, err_size, "%s must be 0(off)/1(peripheral)/2(central)", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cfg_validate_uart_baudrate(const cJSON *value, const char *field,
                                            char *err, size_t err_size)
{
    uint32_t baud = 0;
    if (!cfg_coerce_uint(value, 4000000u, &baud) || baud == 0) {
        snprintf(err, err_size, "%s must be 1..4000000", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cfg_validate_uart_data_bits(const cJSON *value, const char *field,
                                             char *err, size_t err_size)
{
    uint32_t bits = 0;
    if (!cfg_coerce_uint(value, 255, &bits) || bits < 5 || bits > 8) {
        snprintf(err, err_size, "%s must be 5..8", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cfg_validate_uart_stop_bits(const cJSON *value, const char *field,
                                             char *err, size_t err_size)
{
    uint32_t bits = 0;
    if (!cfg_coerce_uint(value, 255, &bits) || bits < 1 || bits > 2) {
        snprintf(err, err_size, "%s must be 1..2", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

static esp_err_t cfg_validate_uart_parity(const cJSON *value, const char *field,
                                          char *err, size_t err_size)
{
    uint32_t parity = 0xFFu;
    if (!cfg_coerce_uint(value, 255, &parity) ||
        (parity != UART_PARITY_DISABLE && parity != UART_PARITY_EVEN && parity != UART_PARITY_ODD)) {
        snprintf(err, err_size, "%s must be 0(none)/2(even)/3(odd)", field);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* 数据面路由掩码:只允许已定义端口位(UART1=1/BLE=2/MQTT=4 的组合,0=不转发) */
static esp_err_t cfg_validate_route_mask(const cJSON *value, const char *field,
                                         char *err, size_t err_size)
{
    uint32_t mask = 0;
    if (!cfg_coerce_uint(value, 0xFFFFFFFFu, &mask) || (mask & ~DATA_PORT_ALL_MASK) != 0) {
        snprintf(err, err_size, "%s must be 0..0x%x", field, (unsigned)DATA_PORT_ALL_MASK);
        return ESP_ERR_INVALID_ARG;
    }
    return ESP_OK;
}

/* ---------------------------------------------------------------------------
 * 伪字段:显式清除 token(前端既有语义:token 留空 = 保持;token_clear = 清除)
 * ------------------------------------------------------------------------- */
static bool cfg_special_token_clear(app_config_t *dst, const cJSON *value)
{
    uint8_t flag = 0;
    if (!cfg_coerce_bool8(value, &flag) || flag == 0) {
        return false;
    }
    if (dst->mqtt_token[0] == '\0') {
        return false;
    }
    dst->mqtt_token[0] = '\0';
    return true;
}

/* 显式清除某个字符串字段(*_clear=true):前端既有语义(留空=保持,clear=清除) */
static bool cfg_special_str_clear(char *dst, size_t dst_size, const cJSON *value)
{
    uint8_t flag = 0;

    if (!cfg_coerce_bool8(value, &flag) || flag == 0) {
        return false;
    }
    if (dst[0] == '\0') {
        return false;   /* 已经是空:无变化 */
    }
    app_copy_string(dst, dst_size, "");
    return true;
}

static bool cfg_special_ap_password_clear(app_config_t *dst, const cJSON *value)
{
    return cfg_special_str_clear(dst->ap_password, sizeof(dst->ap_password), value);
}

static bool cfg_special_sta_password_clear(app_config_t *dst, const cJSON *value)
{
    return cfg_special_str_clear(dst->sta_password, sizeof(dst->sta_password), value);
}

/* 显式清空 STA SSID:前端"留空 = 保持",清空必须走伪字段;
 * 与 mode="ap" 配合才能显式保持纯 AP(见 cfg_net_infer_sta_mode) */
static bool cfg_special_sta_ssid_clear(app_config_t *dst, const cJSON *value)
{
    return cfg_special_str_clear(dst->sta_ssid, sizeof(dst->sta_ssid), value);
}

/* changed 数组去重追加:同一字段可能被表名与别名同时命中,推断步骤也会重复报告 net_mode */
static void cfg_changed_add(cJSON *changed, const char *field)
{
    const cJSON *item = NULL;

    if (changed == NULL || field == NULL) {
        return;
    }
    cJSON_ArrayForEach(item, changed) {
        if (cJSON_IsString(item) && item->valuestring != NULL &&
            strcmp(item->valuestring, field) == 0) {
            return;
        }
    }
    cJSON_AddItemToArray(changed, cJSON_CreateString(field));
}

/* ---------------------------------------------------------------------------
 * 字段表(全量登记):"数据"由本表描述(类型+偏移+校验+生效方式),
 * "行为"由生效域(CFG_DOMAIN_*)承担 —— 新增配置项只需在此加一行。
 *
 * 已接线(本轮 B1/B2):GPIO 输出、MQTT。其余域已登记但未接线,
 * 提交这些字段会被拒绝(ESP_ERR_NOT_SUPPORTED),绝不"改了但不生效"。
 * ------------------------------------------------------------------------- */
static const cfg_field_t s_fields[] = {
    /* ---------- GPIO 输出:热生效,立即作用到引脚 ---------- */
    {.field = "relay_on", .type = CFG_T_BOOL8, .offset = CFG_OFF(relay_on),
     .domain = CFG_DOMAIN_GPIO, .effect = CFG_EFFECT_NONE},
    {.field = "led_on", .type = CFG_T_BOOL8, .offset = CFG_OFF(led_on),
     .domain = CFG_DOMAIN_GPIO, .effect = CFG_EFFECT_NONE},
    /* GPIO 极性/输入极性:改动需重新配置引脚,重启生效(当前无对外 API,产线/内部用) */
    {.field = "relay_active_high", .type = CFG_T_BOOL8, .offset = CFG_OFF(relay_active_high),
     .domain = CFG_DOMAIN_GPIO, .effect = CFG_EFFECT_REBOOT},
    {.field = "led_active_high", .type = CFG_T_BOOL8, .offset = CFG_OFF(led_active_high),
     .domain = CFG_DOMAIN_GPIO, .effect = CFG_EFFECT_REBOOT},
    {.field = "input_active_low", .type = CFG_T_BOOL8, .offset = CFG_OFF(input_active_low),
     .domain = CFG_DOMAIN_GPIO, .effect = CFG_EFFECT_REBOOT},

    /* ---------- MQTT:子系统重启(= 重连),不整机重启 ---------- */
    {.field = "mqtt_backend", .alias = "backend", .type = CFG_T_STR,
     .offset = CFG_OFF(mqtt_backend), .size = CFG_LEN(mqtt_backend),
     .domain = CFG_DOMAIN_MQTT, .effect = CFG_EFFECT_SUBSYS, .empty_keeps = true},
    {.field = "mqtt_host", .alias = "host", .type = CFG_T_STR,
     .offset = CFG_OFF(mqtt_host), .size = CFG_LEN(mqtt_host),
     .domain = CFG_DOMAIN_MQTT, .effect = CFG_EFFECT_SUBSYS, .empty_keeps = true},
    {.field = "mqtt_token", .alias = "token", .type = CFG_T_STR,
     .offset = CFG_OFF(mqtt_token), .size = CFG_LEN(mqtt_token),
     .domain = CFG_DOMAIN_MQTT, .effect = CFG_EFFECT_SUBSYS, .empty_keeps = true},
    {.field = "mqtt_port", .alias = "port", .type = CFG_T_U16,
     .offset = CFG_OFF(mqtt_port), .domain = CFG_DOMAIN_MQTT, .effect = CFG_EFFECT_SUBSYS,
     .validate = cfg_validate_mqtt_port},
    {.field = "mqtt_use_tls", .alias = "use_tls", .type = CFG_T_BOOL8,
     .offset = CFG_OFF(mqtt_use_tls), .domain = CFG_DOMAIN_MQTT, .effect = CFG_EFFECT_SUBSYS},
    {.field = "token_clear", .type = CFG_T_BOOL8, .offset = CFG_OFF(mqtt_token),
     .domain = CFG_DOMAIN_MQTT, .effect = CFG_EFFECT_SUBSYS, .special = cfg_special_token_clear},

    /* ---------- 网络:重启生效(B3 已接线) ---------- */
    {.field = "net_mode", .alias = "mode", .type = CFG_T_U8, .offset = CFG_OFF(net_mode),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .validate = cfg_validate_net_mode,
     .special = cfg_special_net_mode},
    {.field = "ap_ssid", .type = CFG_T_STR, .offset = CFG_OFF(ap_ssid), .size = CFG_LEN(ap_ssid),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true},
    {.field = "ap_password", .type = CFG_T_STR, .offset = CFG_OFF(ap_password), .size = CFG_LEN(ap_password),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_wifi_password},
    {.field = "sta_ssid", .type = CFG_T_STR, .offset = CFG_OFF(sta_ssid), .size = CFG_LEN(sta_ssid),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true},
    {.field = "sta_password", .type = CFG_T_STR, .offset = CFG_OFF(sta_password), .size = CFG_LEN(sta_password),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_wifi_password},
    {.field = "ap_password_clear", .type = CFG_T_BOOL8, .offset = CFG_OFF(ap_password),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .special = cfg_special_ap_password_clear},
    {.field = "sta_password_clear", .type = CFG_T_BOOL8, .offset = CFG_OFF(sta_password),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .special = cfg_special_sta_password_clear},
    {.field = "sta_ssid_clear", .type = CFG_T_BOOL8, .offset = CFG_OFF(sta_ssid),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .special = cfg_special_sta_ssid_clear},

    /* ---------- STA 静态 IP(v9):重启生效 ---------- */
    {.field = "sta_use_static", .type = CFG_T_BOOL8, .offset = CFG_OFF(sta_use_static),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT},
    {.field = "sta_static_ip", .type = CFG_T_STR, .offset = CFG_OFF(sta_static_ip),
     .size = CFG_LEN(sta_static_ip),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_ipv4},
    {.field = "sta_static_gateway", .type = CFG_T_STR, .offset = CFG_OFF(sta_static_gateway),
     .size = CFG_LEN(sta_static_gateway),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_ipv4},
    {.field = "sta_static_netmask", .type = CFG_T_STR, .offset = CFG_OFF(sta_static_netmask),
     .size = CFG_LEN(sta_static_netmask),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_ipv4},
    {.field = "sta_static_dns1", .type = CFG_T_STR, .offset = CFG_OFF(sta_static_dns1),
     .size = CFG_LEN(sta_static_dns1),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_ipv4},
    {.field = "sta_static_dns2", .type = CFG_T_STR, .offset = CFG_OFF(sta_static_dns2),
     .size = CFG_LEN(sta_static_dns2),
     .domain = CFG_DOMAIN_NET, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true,
     .validate = cfg_validate_ipv4},

    /* ---------- UART:子系统重配(B4 接线) ---------- */
    {.field = "uart_baudrate", .type = CFG_T_U32, .offset = CFG_OFF(uart_baudrate),
     .domain = CFG_DOMAIN_UART, .effect = CFG_EFFECT_SUBSYS, .validate = cfg_validate_uart_baudrate},
    {.field = "uart_data_bits", .type = CFG_T_U8, .offset = CFG_OFF(uart_data_bits),
     .domain = CFG_DOMAIN_UART, .effect = CFG_EFFECT_SUBSYS, .validate = cfg_validate_uart_data_bits},
    {.field = "uart_stop_bits", .type = CFG_T_U8, .offset = CFG_OFF(uart_stop_bits),
     .domain = CFG_DOMAIN_UART, .effect = CFG_EFFECT_SUBSYS, .validate = cfg_validate_uart_stop_bits},
    {.field = "uart_parity_mode", .type = CFG_T_U8, .offset = CFG_OFF(uart_parity_mode),
     .domain = CFG_DOMAIN_UART, .effect = CFG_EFFECT_SUBSYS, .validate = cfg_validate_uart_parity},

    /* ---------- 蓝牙:角色(v8 权威字段)重启生效(B6 接线) ---------- */
    {.field = "bt_role", .type = CFG_T_U8, .offset = CFG_OFF(bt_role),
     .domain = CFG_DOMAIN_BT, .effect = CFG_EFFECT_REBOOT, .validate = cfg_validate_bt_role},
    {.field = "bt_device_name", .type = CFG_T_STR, .offset = CFG_OFF(bt_device_name),
     .size = CFG_LEN(bt_device_name),
     .domain = CFG_DOMAIN_BT, .effect = CFG_EFFECT_REBOOT, .empty_keeps = true},

    /* ---------- Web 管理员账号:口令走专用接口(盐+哈希),此处仅登记用户名(B6) ---------- */
    {.field = "web_username", .type = CFG_T_STR, .offset = CFG_OFF(web_username),
     .size = CFG_LEN(web_username),
     .domain = CFG_DOMAIN_WEB_AUTH, .effect = CFG_EFFECT_NONE, .empty_keeps = true},

    /* ---------- 数据面三边路由(1.3-07):立即生效 ---------- */
    {.field = "data_route_uart1", .type = CFG_T_U32, .offset = CFG_OFF(data_route_uart1),
     .domain = CFG_DOMAIN_DATAPIPE, .effect = CFG_EFFECT_NONE, .validate = cfg_validate_route_mask},
    {.field = "data_route_ble", .type = CFG_T_U32, .offset = CFG_OFF(data_route_ble),
     .domain = CFG_DOMAIN_DATAPIPE, .effect = CFG_EFFECT_NONE, .validate = cfg_validate_route_mask},
    {.field = "data_route_mqtt", .type = CFG_T_U32, .offset = CFG_OFF(data_route_mqtt),
     .domain = CFG_DOMAIN_DATAPIPE, .effect = CFG_EFFECT_NONE, .validate = cfg_validate_route_mask},
};

#define CFG_FIELD_COUNT (sizeof(s_fields) / sizeof(s_fields[0]))

/* ---------------------------------------------------------------------------
 * 查表与通用赋值
 * ------------------------------------------------------------------------- */
static const cfg_field_t *cfg_field_lookup(const char *name)
{
    size_t i;

    if (name == NULL) {
        return NULL;
    }
    for (i = 0; i < CFG_FIELD_COUNT; i++) {
        if (strcmp(s_fields[i].field, name) == 0) {
            return &s_fields[i];
        }
        if (s_fields[i].alias != NULL && strcmp(s_fields[i].alias, name) == 0) {
            return &s_fields[i];
        }
    }
    return NULL;
}

const cfg_field_t *config_apply_field(const char *field)
{
    return cfg_field_lookup(field);
}

bool config_apply_field_known(const char *field)
{
    return cfg_field_lookup(field) != NULL;
}

/* 把 patch 中的一项写入 dst;返回 false = 值非法(调用方整体拒绝) */
static bool cfg_field_assign(const cfg_field_t *f, app_config_t *dst, const cJSON *value,
                             bool *changed, char *err, size_t err_size)
{
    uint8_t *base = (uint8_t *)(void *)dst + f->offset;

    *changed = false;
    err[0] = '\0';

    if (f->validate != NULL && f->validate(value, f->field, err, err_size) != ESP_OK) {
        return false;
    }
    if (f->special != NULL) {
        *changed = f->special(dst, value);
        return true;
    }

    switch (f->type) {
    case CFG_T_BOOL8: {
        uint8_t v = 0;
        if (!cfg_coerce_bool8(value, &v)) {
            snprintf(err, err_size, "%s must be bool", f->field);
            return false;
        }
        if (*base != v) {
            *base = v;
            *changed = true;
        }
        return true;
    }
    case CFG_T_U8:
    case CFG_T_U16:
    case CFG_T_U32: {
        uint32_t max = (f->type == CFG_T_U8) ? 0xFFu : (f->type == CFG_T_U16) ? 0xFFFFu : 0xFFFFFFFFu;
        uint32_t v = 0;
        if (!cfg_coerce_uint(value, max, &v)) {
            snprintf(err, err_size, "%s out of range", f->field);
            return false;
        }
        if (f->type == CFG_T_U8) {
            if (*base != (uint8_t)v) {
                *base = (uint8_t)v;
                *changed = true;
            }
        } else if (f->type == CFG_T_U16) {
            uint16_t *p = (uint16_t *)(void *)base;
            if (*p != (uint16_t)v) {
                *p = (uint16_t)v;
                *changed = true;
            }
        } else {
            uint32_t *p = (uint32_t *)(void *)base;
            if (*p != v) {
                *p = v;
                *changed = true;
            }
        }
        return true;
    }
    case CFG_T_STR: {
        char tmp[CFG_STR_MAX + 1];
        if (f->size == 0 || f->size > sizeof(tmp)) {
            snprintf(err, err_size, "%s size out of range", f->field);
            return false;
        }
        if (!cfg_coerce_str(value, tmp, f->size)) {
            snprintf(err, err_size, "%s must be string", f->field);
            return false;
        }
        if (f->empty_keeps && tmp[0] == '\0') {
            return true; /* 空串 = 保持原值(token/host/backend 等既有语义) */
        }
        if (strncmp((const char *)base, tmp, f->size) != 0) {
            memcpy(base, tmp, f->size);
            ((char *)base)[f->size - 1] = '\0';
            *changed = true;
        }
        return true;
    }
    default:
        snprintf(err, err_size, "%s unsupported type", f->field);
        return false;
    }
}

/* ---------------------------------------------------------------------------
 * 生效域:一个域一份实现。未接线的域拒绝写入,避免"改了配置但不生效"。
 * ------------------------------------------------------------------------- */
bool config_apply_domain_wired(cfg_domain_t domain)
{
    switch (domain) {
    case CFG_DOMAIN_NONE:
    case CFG_DOMAIN_GPIO:
    case CFG_DOMAIN_BT: /* 蓝牙为重启生效:仅落盘,由调用方按 effect 重启 */
    case CFG_DOMAIN_MQTT:
    case CFG_DOMAIN_NET: /* B3(1.3版本优化.md 1.3-09):网络为重启生效,仅落盘 */
    case CFG_DOMAIN_WEB_AUTH: /* B6:仅用户名走字段表(口令是盐+SHA256 的专用接口) */
    case CFG_DOMAIN_DATAPIPE: /* 1.3-07:数据面路由,立即生效 */
        return true;
    default:
        return false; /* UART 待 B4 评审结论(结论:不迁移) */
    }
}

esp_err_t config_apply_domain_apply(cfg_domain_t domain)
{
    switch (domain) {
    case CFG_DOMAIN_NONE:
        return ESP_OK;

    case CFG_DOMAIN_GPIO: {
        bool relay_on, led_on, relay_active_high, led_active_high;

        app_config_lock();
        relay_on = (s_config.relay_on != 0);
        led_on = (s_config.led_on != 0);
        relay_active_high = (s_config.relay_active_high != 0);
        led_active_high = (s_config.led_active_high != 0);
        app_config_unlock();

        app_apply_output(app_relay_gpio(), relay_active_high, relay_on);
        app_apply_output(app_led_gpio(), led_active_high, led_on);
        return ESP_OK;
    }

    case CFG_DOMAIN_MQTT:
        /* 1.3-43:配置变更 = 用户明确要重连,先清致命位再调度 */
        app_mqtt_clear_fatal();
        return app_mqtt_schedule_restart() ? ESP_OK : ESP_FAIL;

    case CFG_DOMAIN_BT:
        /* 重启生效:角色/设备名已落盘,运行时动作由重启完成(避免热重配,见第 21 项) */
        return ESP_OK;

    case CFG_DOMAIN_NET:
        /* B3(1.3版本优化.md 1.3-09):网络配置为整机重启生效,本层只负责落盘;
         * 是否提示/触发重启由调用方按 effect_out 决定(HTTP handler 返回 restart_required) */
        return ESP_OK;

    case CFG_DOMAIN_WEB_AUTH:
        /* B6:用户名即时生效(effect=NONE),无需运行时动作;
         * 口令是"盐+SHA256"派生值、不是普通字段,保留专用接口 PUT /api/v1/config/web */
        return ESP_OK;

    case CFG_DOMAIN_DATAPIPE:
        /* 1.3-07:三边路由立即生效(回环开启时 data_pipe_apply_routes 仅存档,
         * 关闭回环后生效) */
        app_config_lock();
        data_pipe_apply_routes(s_config.data_route_uart1,
                               s_config.data_route_ble,
                               s_config.data_route_mqtt);
        app_config_unlock();
        return ESP_OK;

    default:
        return ESP_ERR_NOT_SUPPORTED;
    }
}

/* ---------------------------------------------------------------------------
 * 唯一写入入口
 * ------------------------------------------------------------------------- */
esp_err_t config_apply_patch(const cJSON *patch, cJSON **changed_out, cfg_effect_t *effect_out)
{
    /* 配置结构 ~550B:静态副本避免 httpd 任务栈压力(与 app_config_save 同风格) */
    static app_config_t s_next;
    bool domain_changed[CFG_DOMAIN_MAX];
    bool net_touched = false;   /* patch 里出现过 net 域字段(无论值是否变化) */
    cfg_effect_t effect = CFG_EFFECT_NONE;
    cJSON *changed = NULL;
    const cJSON *item = NULL;
    char err_msg[64];
    bool any_change = false;
    esp_err_t err = ESP_OK;
    int d;

    if (changed_out != NULL) {
        *changed_out = NULL;
    }
    if (effect_out != NULL) {
        *effect_out = CFG_EFFECT_NONE;
    }
    if (!cJSON_IsObject(patch)) {
        return ESP_ERR_INVALID_ARG;
    }

    changed = cJSON_CreateArray();
    if (changed == NULL) {
        ESP_LOGW(TAG, "changed-list alloc failed, continue without reporting");
    }
    memset(domain_changed, 0, sizeof(domain_changed));

    /* 读-改-写在同一临界区;配置锁是递归锁,app_config_save 内部再取一次是安全的 */
    app_config_lock();
    memcpy(&s_next, &s_config, sizeof(s_next));

    cJSON_ArrayForEach(item, patch) {
        const cfg_field_t *f;
        bool field_changed = false;

        if (item->string == NULL) {
            continue;
        }
        f = cfg_field_lookup(item->string);
        if (f == NULL) {
            /* 未知字段忽略:与既有 handler"只挑已知字段"的行为一致 */
            ESP_LOGD(TAG, "ignore unknown field: %s", item->string);
            continue;
        }
        if (f->domain == CFG_DOMAIN_NET) {
            /* 提交即视为"用户正在配置网络":即使值与现状相同也要做一致性推断
             * (否则"SSID 已存但模式为纯 AP"这一坏状态按保存也无法自愈) */
            net_touched = true;
        }
        err_msg[0] = '\0';
        if (!cfg_field_assign(f, &s_next, item, &field_changed, err_msg, sizeof(err_msg))) {
            ESP_LOGW(TAG, "reject %s: %s", item->string, err_msg[0] ? err_msg : "invalid value");
            err = ESP_ERR_INVALID_ARG; /* 不做部分提交:任一字段非法即整体拒绝 */
            goto done;
        }
        if (!field_changed) {
            continue;
        }
        any_change = true;
        if (f->domain < CFG_DOMAIN_MAX) {
            domain_changed[f->domain] = true;
        }
        if (f->effect > effect) {
            effect = f->effect;
        }
        cfg_changed_add(changed, f->field);
    }

    /* net 域一致性推断(见 cfg_net_infer_sta_mode):只在本次 patch 触及 net 域时执行
     * ("触及"= 提交了 net 域字段,含值未变的情形),避免"只改 GPIO 却把历史遗留的 net_mode 顺手改掉" */
    if (net_touched && cfg_net_infer_sta_mode(&s_next)) {
        ESP_LOGW(TAG, "net_mode %u -> %u: sta_ssid '%s' saved without STA mode, auto-enable STA (AP+STA)",
                 (unsigned)s_config.net_mode, (unsigned)s_next.net_mode, s_next.sta_ssid);
        any_change = true;
        domain_changed[CFG_DOMAIN_NET] = true;
        cfg_changed_add(changed, "net_mode");
        if (effect < CFG_EFFECT_REBOOT) {
            effect = CFG_EFFECT_REBOOT;   /* 模式变化需整机重启(与 net 域语义一致) */
        }
    }

    /* 跨字段校验:改 SSID 必须同时提供合法口令 */
    if (net_touched && !cfg_validate_net_cross(&s_config, &s_next, err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "reject net patch: %s", err_msg);
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }

    /* 跨字段校验:启用静态 IP 时必须提供合法 IP/网关/掩码 */
    if (net_touched && !cfg_validate_static_ip_cross(&s_config, &s_next, err_msg, sizeof(err_msg))) {
        ESP_LOGW(TAG, "reject net patch: %s", err_msg);
        err = ESP_ERR_INVALID_ARG;
        goto done;
    }

    if (!any_change) {
        /* 值与现值相同:不落盘、不生效(连 NVS 读回比较都省掉) */
        goto done;
    }

    for (d = 0; d < CFG_DOMAIN_MAX; d++) {
        if (domain_changed[d] && !config_apply_domain_wired((cfg_domain_t)d)) {
            ESP_LOGW(TAG, "domain %d not wired, reject patch", d);
            err = ESP_ERR_NOT_SUPPORTED;
            goto done;
        }
    }

    /* 先落盘成功、再提交内存:失败即回滚,避免"接口报成功、重启后回旧值" */
    err = app_config_save(&s_next);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config save failed: %s", esp_err_to_name(err));
        goto done;
    }
    memcpy(&s_config, &s_next, sizeof(s_next));

done:
    app_config_unlock();

    /* 生效动作在锁外执行:重连/IO 不应持有配置锁 */
    if (err == ESP_OK && any_change) {
        for (d = 0; d < CFG_DOMAIN_MAX; d++) {
            if (domain_changed[d]) {
                esp_err_t aerr = config_apply_domain_apply((cfg_domain_t)d);
                if (aerr != ESP_OK) {
                    ESP_LOGW(TAG, "domain %d apply: %s", d, esp_err_to_name(aerr));
                }
            }
        }
    }

    if (err == ESP_OK && changed_out != NULL && changed != NULL) {
        *changed_out = changed;
        changed = NULL; /* 所有权移交调用方 */
    }
    if (changed != NULL) {
        cJSON_Delete(changed);
    }
    if (err == ESP_OK && effect_out != NULL) {
        *effect_out = effect;
    }
    return err;
}

esp_err_t config_apply_json(const char *json_body, cJSON **changed_out, cfg_effect_t *effect_out)
{
    cJSON *patch;
    esp_err_t err;

    if (json_body == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    patch = cJSON_Parse(json_body);
    if (patch == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    err = config_apply_patch(patch, changed_out, effect_out);
    cJSON_Delete(patch);
    return err;
}

/* ---------------------------------------------------------------------------
 * 仅校验:不修改配置、不落盘、不生效。
 * 用于前端"保存前当场校验",把统一层的字段/跨字段规则暴露成可读的错语。
 * ------------------------------------------------------------------------- */
esp_err_t config_apply_json_validate(const char *json_body, char *err, size_t err_size)
{
    cJSON *patch = NULL;
    cJSON *item = NULL;
    app_config_t next;
    bool net_touched = false;
    bool domain_changed[CFG_DOMAIN_MAX] = {false};
    char err_msg[128];
    esp_err_t rv = ESP_OK;

    if (err != NULL && err_size > 0) {
        err[0] = '\0';
    }
    if (json_body == NULL) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "empty request");
        }
        return ESP_ERR_INVALID_ARG;
    }
    patch = cJSON_Parse(json_body);
    if (patch == NULL || !cJSON_IsObject(patch)) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "invalid json");
        }
        if (patch != NULL) {
            cJSON_Delete(patch);
        }
        return ESP_ERR_INVALID_ARG;
    }

    app_config_lock();
    memcpy(&next, &s_config, sizeof(next));

    cJSON_ArrayForEach(item, patch) {
        const cfg_field_t *f;
        bool field_changed = false;

        if (item->string == NULL) {
            continue;
        }
        f = cfg_field_lookup(item->string);
        if (f == NULL) {
            continue;
        }
        if (f->domain == CFG_DOMAIN_NET) {
            net_touched = true;
        }
        err_msg[0] = '\0';
        if (!cfg_field_assign(f, &next, item, &field_changed, err_msg, sizeof(err_msg))) {
            ESP_LOGW(TAG, "validate reject %s: %s",
                     item->string, err_msg[0] ? err_msg : "invalid value");
            if (err != NULL && err_size > 0) {
                snprintf(err, err_size, "%s", err_msg[0] ? err_msg : "invalid value");
            }
            rv = ESP_ERR_INVALID_ARG;
            goto done;
        }
        if (!field_changed) {
            continue;
        }
        if (f->domain < CFG_DOMAIN_MAX) {
            domain_changed[f->domain] = true;
        }
    }

    if (net_touched && cfg_net_infer_sta_mode(&next)) {
        domain_changed[CFG_DOMAIN_NET] = true;
    }

    {
        int d;
        for (d = 0; d < CFG_DOMAIN_MAX; d++) {
            if (domain_changed[d] && !config_apply_domain_wired((cfg_domain_t)d)) {
                if (err != NULL && err_size > 0) {
                    snprintf(err, err_size, "domain %d not wired", d);
                }
                rv = ESP_ERR_NOT_SUPPORTED;
                goto done;
            }
        }
    }

    if (net_touched && !cfg_validate_net_cross(&s_config, &next, err_msg, sizeof(err_msg))) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "%s", err_msg);
        }
        rv = ESP_ERR_INVALID_ARG;
        goto done;
    }

    if (net_touched && !cfg_validate_static_ip_cross(&s_config, &next, err_msg, sizeof(err_msg))) {
        if (err != NULL && err_size > 0) {
            snprintf(err, err_size, "%s", err_msg);
        }
        rv = ESP_ERR_INVALID_ARG;
        goto done;
    }

done:
    app_config_unlock();
    cJSON_Delete(patch);
    return rv;
}

/* ---------------------------------------------------------------------------
 * 启动期自检:字段表重名/越界/尺寸非法(只告警,不阻塞启动)
 * ------------------------------------------------------------------------- */

/* 字段在结构体里实际占用的字节数(自检边界判断用)。
 * 注意:数值字段的 f->size 约定为 0,必须按类型精确取值——否则像 bt_role
 * (uint8_t 且位于结构体最末尾)会被按 uint32 估算而误报"out of struct bounds"。 */
static size_t cfg_field_bytes(const cfg_field_t *f)
{
    if (f->type == CFG_T_STR) {
        return f->size;
    }
    switch (f->type) {
    case CFG_T_BOOL8:
    case CFG_T_U8:
        return sizeof(uint8_t);
    case CFG_T_U16:
        return sizeof(uint16_t);
    case CFG_T_U32:
        return sizeof(uint32_t);
    default:
        break;
    }
    return sizeof(uint8_t);
}

void config_apply_init(void)
{
    size_t i, j;
    unsigned problems = 0;

    for (i = 0; i < CFG_FIELD_COUNT; i++) {
        const cfg_field_t *f = &s_fields[i];

        if (f->field == NULL || f->field[0] == '\0') {
            ESP_LOGE(TAG, "field[%u]: empty name", (unsigned)i);
            problems++;
            continue;
        }
        if (f->type == CFG_T_STR) {
            if (f->size == 0 || f->size > CFG_STR_MAX + 1) {
                ESP_LOGE(TAG, "field %s: bad size %u", f->field, (unsigned)f->size);
                problems++;
            }
        } else if (f->size != 0) {
            ESP_LOGE(TAG, "field %s: numeric field must have size 0", f->field);
            problems++;
        }
        if (f->offset + cfg_field_bytes(f) > sizeof(app_config_t)) {
            ESP_LOGE(TAG, "field %s: out of struct bounds (offset=%u bytes=%u struct=%u)",
                     f->field, (unsigned)f->offset, (unsigned)cfg_field_bytes(f),
                     (unsigned)sizeof(app_config_t));
            problems++;
        }
        for (j = i + 1; j < CFG_FIELD_COUNT; j++) {
            const cfg_field_t *g = &s_fields[j];
            bool clash = (strcmp(f->field, g->field) == 0);

            if (!clash && f->alias != NULL && g->alias != NULL) {
                clash = (strcmp(f->alias, g->alias) == 0);
            }
            if (!clash && f->alias != NULL) {
                clash = (strcmp(f->alias, g->field) == 0);
            }
            if (!clash && g->alias != NULL) {
                clash = (strcmp(f->field, g->alias) == 0);
            }
            if (clash) {
                ESP_LOGE(TAG, "duplicate field/alias: %s <-> %s", f->field, g->field);
                problems++;
            }
        }
    }

    ESP_LOGI(TAG, "config_apply ready: %u fields, %u problem(s); wired domains: gpio/mqtt/net/bt/web_auth/datapipe",
             (unsigned)CFG_FIELD_COUNT, problems);
}

