#include "cmd_v1_id.h"

#include <math.h>
#include <stdint.h>
#include <string.h>

void app_cmd_v1_id_reset(app_cmd_v1_id_t *id)
{
    if (id == NULL) {
        return;
    }
    id->kind = APP_CMD_V1_ID_NONE;
    id->text[0] = '\0';
    id->len = 0;
    id->number = 0.0;
}

/* 严格 UTF-8:拒绝 overlong、代理项、>U+10FFFF 和截断序列。 */
static bool app_cmd_v1_id_utf8_valid(const char *s, size_t len)
{
    const unsigned char *p = (const unsigned char *)s;
    size_t i = 0;

    while (i < len) {
        unsigned char c = p[i];
        size_t need;
        uint32_t cp;

        if (c < 0x80) {
            i++;
            continue;
        }
        if ((c & 0xE0) == 0xC0) {
            need = 1;
            cp = (uint32_t)(c & 0x1F);
        } else if ((c & 0xF0) == 0xE0) {
            need = 2;
            cp = (uint32_t)(c & 0x0F);
        } else if ((c & 0xF8) == 0xF0) {
            need = 3;
            cp = (uint32_t)(c & 0x07);
        } else {
            return false;   /* 孤立续字节或 5/6 字节前缀 */
        }
        if (i + need >= len) {
            return false;   /* 截断序列 */
        }
        for (size_t k = 1; k <= need; k++) {
            unsigned char cc = p[i + k];

            if ((cc & 0xC0) != 0x80) {
                return false;
            }
            cp = (cp << 6) | (uint32_t)(cc & 0x3F);
        }
        if ((need == 1 && cp < 0x80) ||
            (need == 2 && cp < 0x800) ||
            (need == 3 && cp < 0x10000)) {
            return false;   /* overlong */
        }
        if (cp >= 0xD800 && cp <= 0xDFFF) {
            return false;   /* 代理项 */
        }
        if (cp > 0x10FFFF) {
            return false;
        }
        i += need + 1;
    }
    return true;
}

bool app_cmd_v1_id_set_string(app_cmd_v1_id_t *id, const char *s)
{
    size_t len;

    if (id == NULL) {
        return false;
    }
    app_cmd_v1_id_reset(id);
    if (s == NULL) {
        return false;
    }
    len = strlen(s);
    if (len == 0 || len > APP_CMD_V1_ID_MAX_BYTES) {
        return false;
    }
    if (!app_cmd_v1_id_utf8_valid(s, len)) {
        return false;
    }
    memcpy(id->text, s, len);
    id->text[len] = '\0';
    id->len = len;
    id->kind = APP_CMD_V1_ID_STRING;
    return true;
}

bool app_cmd_v1_id_set_number(app_cmd_v1_id_t *id, double v)
{
    if (id == NULL) {
        return false;
    }
    app_cmd_v1_id_reset(id);
    if (!isfinite(v)) {
        return false;
    }
    id->kind = APP_CMD_V1_ID_NUMBER;
    id->number = v;
    return true;
}

bool app_cmd_v1_raw_has_nul_escape(const char *buf, size_t len)
{
    const unsigned char *p = (const unsigned char *)buf;
    bool in_string = false;
    size_t i = 0;

    if (buf == NULL || len == 0) {
        return false;
    }
    while (i < len) {
        unsigned char c = p[i];

        if (!in_string) {
            if (c == '"') {
                in_string = true;
            }
            i++;
            continue;
        }
        if (c == '"') {
            in_string = false;
            i++;
            continue;
        }
        if (c != '\\') {
            i++;
            continue;
        }

        /* 字符串内的转义:至少还要有 1 个字节。 */
        if (i + 1 >= len) {
            return false;
        }
        if (p[i + 1] == 'u') {
            unsigned value = 0;
            bool hex_ok = true;

            /* \u 后必须紧跟 4 个十六进制字符;被截断时按非法转义处理(解析器负责拒绝),
             * 这里只保证不越界。 */
            if (i + 5 >= len) {
                i += 2;
                continue;
            }
            for (size_t k = 0; k < 4; k++) {
                unsigned char h = p[i + 2 + k];
                unsigned digit;

                if (h >= '0' && h <= '9') {
                    digit = (unsigned)(h - '0');
                } else if (h >= 'a' && h <= 'f') {
                    digit = (unsigned)(h - 'a') + 10u;
                } else if (h >= 'A' && h <= 'F') {
                    digit = (unsigned)(h - 'A') + 10u;
                } else {
                    hex_ok = false;
                    break;
                }
                value = (value << 4) | digit;
            }
            if (hex_ok && value == 0) {
                return true;
            }
            i += hex_ok ? 6 : 2;
            continue;
        }
        /* 其他转义(\" \\ \/ \n ...):跳过两字节。\\u0000 在第一步就吃掉了
         * 两个反斜杠,后面的 u0000 是普通文本。 */
        i += 2;
    }
    return false;
}
