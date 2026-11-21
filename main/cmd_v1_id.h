#pragma once

#include <stdbool.h>
#include <stddef.h>

/* v1 请求 id 的值语义表示(PROTO-ID-01)。
 *
 * 纯 C 模块:不依赖 cJSON / ESP-IDF,供 ble_cmd.c、mqtt.c 和 host 测试共用。
 * NUL 截断的防线不在 set_string(它只收到已解析的 C 字符串),而在调用点
 * 对原始 payload 调用 app_cmd_v1_raw_has_nul_escape():cJSON 的
 * valuestring 遇 \u0000 会在 NUL 处截断,必须先拒绝原始文本。 */

#define APP_CMD_V1_ID_MAX_BYTES 64

typedef enum {
    APP_CMD_V1_ID_NONE = 0,
    APP_CMD_V1_ID_STRING,
    APP_CMD_V1_ID_NUMBER,
} app_cmd_v1_id_kind_t;

typedef struct {
    app_cmd_v1_id_kind_t kind;
    char text[APP_CMD_V1_ID_MAX_BYTES + 1];   /* STRING:原样 UTF-8,NUL 结尾 */
    size_t len;                               /* STRING:字节数(1..64) */
    double number;                            /* NUMBER:有限值 */
} app_cmd_v1_id_t;

void app_cmd_v1_id_reset(app_cmd_v1_id_t *id);

/* NULL / 空串 / 超过 64 字节 / 非严格 UTF-8 一律失败;失败后 id 保持 NONE。 */
bool app_cmd_v1_id_set_string(app_cmd_v1_id_t *id, const char *s);

/* 非有限 double(NaN/Infinity)失败;失败后 id 保持 NONE。
 * 不做整数转换、不格式化:序列化交给 cJSON 的 double 打印。 */
bool app_cmd_v1_id_set_number(app_cmd_v1_id_t *id, double v);

/* 扫描原始 JSON 文本中"字符串内部"的有效 \u0000 转义(十六进制大小写不敏感)。
 * - 字符串外的反斜杠不参与匹配;
 * - \\u0000(字面反斜杠 + u0000)不命中;
 * - 所有读取按 len 边界检查,转义被截断时不越界。 */
bool app_cmd_v1_raw_has_nul_escape(const char *buf, size_t len);
