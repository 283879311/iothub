#include <ctype.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "form_url.h"

/* 本地有界拷贝(本文件不依赖 common.c/IDF,保持"可被宿主单测直接编译") */
static void form_copy_n(char *dst, size_t dst_size, const char *src, size_t src_len)
{
    size_t i = 0;

    if (dst_size == 0) {
        return;
    }
    while (i < src_len && src[i] != '\0' && i + 1 < dst_size) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

/* 解一个 '+' 或 %XX 原子(成功则把结果写入 *out 并前进 *in,返回 true) */
static bool form_decode_atom(const char **in, char *out)
{
    const char *r = *in;

    if (*r == '+') {
        *out = ' ';
        *in = r + 1;
        return true;
    }
    if (*r == '%' && isxdigit((unsigned char)r[1]) && isxdigit((unsigned char)r[2])) {
        char hex[3];

        hex[0] = r[1];
        hex[1] = r[2];
        hex[2] = '\0';
        *out = (char)strtol(hex, NULL, 16);
        *in = r + 3;
        return true;
    }
    return false;
}

void app_url_decode(char *s)
{
    const char *read = s;
    char *write = s;

    while (*read != '\0') {
        if (form_decode_atom(&read, write)) {
            write++;
            continue;
        }
        *write++ = *read++;
    }
    *write = '\0';
}

/* 就地解码整个表单体:只解码"值"(键名不动),并把缩短后的内容**左移压实**。
 * 注意:不能只把 '&' 临时置 NUL 再逐字段就地解码 —— 值变短(如 a%3Db → a=b)时会在
 * body 中间留下空字节,把后续字段整段截断(宿主单测 tools/form_url 已覆盖该回归)。
 * 已知边界:值里出现 %00 会被当作字符串结束(表单值本就是 C 字符串,无法表达 NUL)。 */
void app_form_decode_all(char *body)
{
    const char *read = body;
    char *write = body;
    bool in_value = false;

    while (*read != '\0') {
        if (*read == '=') {
            in_value = true;
            *write++ = *read++;
            continue;
        }
        if (*read == '&') {
            in_value = false;
            *write++ = *read++;
            continue;
        }
        if (in_value && form_decode_atom(&read, write)) {
            write++;
            continue;
        }
        *write++ = *read++;
    }
    *write = '\0';
}

bool app_form_lookup(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;

    while (*p != '\0') {
        const char *amp = strchr(p, '&');
        size_t part_len = (amp != NULL) ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', part_len);

        if (eq != NULL && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            /* 只取到本字段结尾:不能拷到字符串末尾 —— 否则会把后续 "&key=value" 一起带走
             * (实测会让 username 变成 "admin&password=..." → 登录必然失败) */
            form_copy_n(out, out_size, eq + 1, part_len - (size_t)(eq - p) - 1);
            return true;
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

/* 先切字段、再解码(标准 urlencoded 语义;见 form_url.h 的说明)。
 * 与上面"先整体解码再切"的组合相比,差别只在含 `%26` 的值上 —— 那正是真实用户
 * 口令里出现 `&` 时的场景(实测:被截断成前半段 → 永远登不进去)。 */
bool app_form_lookup_raw(const char *body, const char *key, char *out, size_t out_size)
{
    size_t key_len = strlen(key);
    const char *p = body;

    if (out == NULL || out_size == 0) {
        return false;
    }
    out[0] = '\0';

    while (p != NULL && *p != '\0') {
        const char *amp = strchr(p, '&');
        size_t part_len = (amp != NULL) ? (size_t)(amp - p) : strlen(p);
        const char *eq = memchr(p, '=', part_len);

        if (eq != NULL && (size_t)(eq - p) == key_len && strncmp(p, key, key_len) == 0) {
            form_copy_n(out, out_size, eq + 1, part_len - (size_t)(eq - p) - 1);
            app_url_decode(out);   /* 只解码本字段的值:'+'→空格、%XX→字节 */
            return true;
        }
        if (amp == NULL) {
            break;
        }
        p = amp + 1;
    }
    return false;
}

bool app_cookie_value(const char *cookie_header, const char *name,
                      char *out, size_t out_size)
{
    size_t name_len;
    const char *pos;
    size_t i = 0;

    if (cookie_header == NULL || name == NULL || out == NULL || out_size == 0) {
        return false;
    }
    name_len = strlen(name);
    if (name_len == 0) {
        return false;
    }

    pos = strstr(cookie_header, name);
    while (pos != NULL) {
        size_t off = (size_t)(pos - cookie_header);
        /* 键名须在串首,或紧跟 "; " / ";"(RFC 6265 分隔符可无空格) */
        bool at_boundary = (off == 0) ||
                           (off >= 1 && cookie_header[off - 1] == ';') ||
                           (off >= 2 && cookie_header[off - 1] == ' ' && cookie_header[off - 2] == ';');

        if (at_boundary && pos[name_len] == '=') {
            break;
        }
        pos = strstr(pos + 1, name);
    }
    if (pos == NULL) {
        return false;
    }

    pos += name_len + 1;                     /* 跳过 "name=" */
    while (pos[i] != '\0' && pos[i] != ';' && i + 1 < out_size) {
        out[i] = pos[i];
        i++;
    }
    out[i] = '\0';
    return out[0] != '\0';
}
