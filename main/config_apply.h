#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "cJSON.h"
#include "esp_err.h"

#include "config.h"

/* ============================================================================
 * 统一配置写入层(唯一写入点)
 *
 * 背景:此前配置写入散落在 7 个文件、17 处直接赋值 + 13 处各自 app_config_save(),
 * 且"改完怎么生效"各写一遍。本层把"校验 → 比较 → 落盘 → 生效"收敛到一处:
 *
 *   任一链路(HTTP/MQTT/BLE/UART) --patch--> config_apply_patch()
 *        → 字段表(类型+偏移+校验) → copy-on-write 到副本 → diff
 *        → 只对变化项落盘一次 → 按"生效域"执行生效动作
 *
 * 设计要点:
 *   - 字段表是"数据"(类型/偏移/校验),生效域是"行为"(一个域一份实现),两者正交;
 *     新增配置项只需在表里加一行,无需改任何 handler。
 *   - 生效方式为三态:立即 / 子系统重启 / 整机重启(原 http_mqtt.c 的 need_restart
 *     实为"MQTT 子系统重连",借此显式区分)。
 *   - 未接线的生效域一律拒绝(返回 ESP_ERR_NOT_SUPPORTED),绝不"改了配置但不生效"。
 * ========================================================================== */

/* 生效方式(三态) */
typedef enum {
    CFG_EFFECT_NONE = 0, /* 立即生效(输出/状态类) */
    CFG_EFFECT_SUBSYS,   /* 子系统重启(如 MQTT 重连、UART 重配) */
    CFG_EFFECT_REBOOT,   /* 整机重启(网络/蓝牙类) */
} cfg_effect_t;

/* 字段类型:决定统一的赋值/比较方式,免去每个字段写一个函数 */
typedef enum {
    CFG_T_BOOL8 = 0,
    CFG_T_U8,
    CFG_T_U16,
    CFG_T_U32,
    CFG_T_STR,
} cfg_type_t;

/* 生效域:一个域一份生效实现,字段只声明归属 */
typedef enum {
    CFG_DOMAIN_NONE = 0, /* 无需生效动作 */
    CFG_DOMAIN_GPIO,     /* 已接线:输出立即应用 */
    CFG_DOMAIN_MQTT,     /* 已接线:异步重连 */
    CFG_DOMAIN_UART,     /* 预留(B4 评审结论:不迁移) */
    CFG_DOMAIN_BT,       /* 已接线(B6):重启生效 */
    CFG_DOMAIN_NET,      /* 已接线(B3):重启生效 */
    CFG_DOMAIN_WEB_AUTH, /* 已接线(B6) */
    CFG_DOMAIN_DATAPIPE, /* 已接线(1.3-07):数据面路由,立即生效 */
    CFG_DOMAIN_MAX,
} cfg_domain_t;

typedef esp_err_t (*cfg_validate_fn)(const cJSON *value, const char *field,
                                     char *err, size_t err_size);
/* 伪字段专用赋值(如 token_clear);返回 true = 值确有变化 */
typedef bool (*cfg_assign_fn)(app_config_t *dst, const cJSON *value);

typedef struct {
    const char     *field;       /* 规范字段名(命令层/长名) */
    const char     *alias;       /* 兼容别名(HTTP 既有短名),可为 NULL */
    cfg_type_t      type;
    size_t          offset;      /* offsetof(app_config_t, x) */
    size_t          size;        /* CFG_T_STR 的缓冲长度,其余填 0 */
    cfg_domain_t    domain;
    cfg_effect_t    effect;
    bool            empty_keeps; /* 空串/无值 = 保持原值(如 token) */
    cfg_validate_fn validate;    /* 可为 NULL */
    cfg_assign_fn   special;     /* 非 NULL 时覆盖通用赋值(伪字段) */
} cfg_field_t;

/* 启动期调用一次:校验字段表自身(重名/越界),失败仅告警不阻塞启动 */
void config_apply_init(void);

/* 内核:patch 为 JSON 对象;changed_out 为变化字段名数组(调用方负责 cJSON_Delete) */
esp_err_t config_apply_patch(const cJSON *patch, cJSON **changed_out, cfg_effect_t *effect_out);
/* 便捷入口:直接吃 HTTP body(内部只解析一次,替代 app_json_find_* 的多次重复解析) */
esp_err_t config_apply_json(const char *json_body, cJSON **changed_out, cfg_effect_t *effect_out);
/* 仅校验入口:不修改配置、不落盘、不生效;失败时 err 写入可读错语 */
esp_err_t config_apply_json_validate(const char *json_body, char *err, size_t err_size);

/* 按字段名或别名查表(未登记返回 NULL) */
const cfg_field_t *config_apply_field(const char *field);
bool config_apply_field_known(const char *field);

/* 执行某个生效域(供内核调用;将来命令层也可显式调用)。未接线域返回 ESP_ERR_NOT_SUPPORTED */
esp_err_t config_apply_domain_apply(cfg_domain_t domain);
bool config_apply_domain_wired(cfg_domain_t domain);
