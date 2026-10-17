#include <stdio.h>
#include <string.h>

#include "json_escape.h"

/* "\u00XX" 共 6 字节,加 NUL 收尾 => 缓冲 7+1 */
#define JSON_ESCAPE_SEQ_BUF 8

const char *app_json_escape_seq(char ch, char *buf)
{
    switch (ch) {
    case '\\':
        return "\\\\";
    case '"':
        return "\\\"";
    case '/':
        return "\\/";
    case '\b':
        return "\\b";
    case '\f':
        return "\\f";
    case '\n':
        return "\\n";
    case '\r':
        return "\\r";
    case '\t':
        return "\\t";
    default:
        /* 其余 C0 控制字符按 RFC 8259 用 \uXXXX 转义,不再丢字替换为 '?'。
         * >= 0x80 的裸字节同样转义:payload 是字节流,直接落 JSON 会产出
         * 非法 UTF-8(实测:hex 发送 AA 55.. 后 /api/v1/config/uart 返回体
         * 带裸 0xAA,JSON 解析失败)。 */
        if (buf != NULL && ((unsigned char)ch < 0x20 || (unsigned char)ch >= 0x80)) {
            snprintf(buf, JSON_ESCAPE_SEQ_BUF, "\\u%04X", (unsigned)(unsigned char)ch);
            return buf;
        }
        return NULL;
    }
}

void app_json_escape_string(char *dst, size_t dst_size, const char *src)
{
    size_t index = 0;

    if (dst_size == 0) {
        return;
    }

    while (*src != '\0' && index + 1 < dst_size) {
        char unicode_escape[JSON_ESCAPE_SEQ_BUF];
        const char *replacement = app_json_escape_seq(*src, unicode_escape);
        char ch = *src++;

        if (replacement != NULL) {
            while (*replacement != '\0' && index + 1 < dst_size) {
                dst[index++] = *replacement++;
            }
            continue;
        }

        dst[index++] = ch;
    }

    dst[index] = '\0';
}

bool app_json_escape_stream_step(const char *src, size_t *cursor,
                                 char *out, size_t out_size, size_t *out_len)
{
    size_t pos;
    size_t written = 0;

    if (out_len != NULL) {
        *out_len = 0;
    }
    if (src == NULL || out == NULL || out_size == 0) {
        return false;
    }

    pos = (cursor != NULL) ? *cursor : 0;

    while (src[pos] != '\0') {
        char unicode_escape[JSON_ESCAPE_SEQ_BUF];
        const char *seq = app_json_escape_seq(src[pos], unicode_escape);
        const char *emit = (seq != NULL) ? seq : &src[pos];
        size_t emit_len = (seq != NULL) ? strlen(seq) : 1;

        if (written + emit_len > out_size) {
            /* 本片装不下:整个转义原子留给下一片,保证不切断转义 */
            break;
        }

        memcpy(out + written, emit, emit_len);
        written += emit_len;
        pos++;
    }

    if (cursor != NULL) {
        *cursor = pos;
    }
    if (out_len != NULL) {
        *out_len = written;
    }
    return src[pos] == '\0';
}
