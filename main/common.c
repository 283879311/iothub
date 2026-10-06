#include <stdio.h>
#include <string.h>

#include "psa/crypto.h"

#include "constants.h"
#include "common.h"

void app_web_hash_password(const uint8_t *salt, size_t salt_len,
                           const char *password, uint8_t *out_hash)
{
    psa_hash_operation_t op = PSA_HASH_OPERATION_INIT;
    psa_status_t status;
    size_t hash_len = 0;

    /* PSA 初始化幂等,可重复调用;首次调用开销可接受(仅发生在认证/改密路径) */
    status = psa_crypto_init();
    if (status != PSA_SUCCESS) {
        memset(out_hash, 0, 32);
        return;
    }

    status = psa_hash_setup(&op, PSA_ALG_SHA_256);
    if (status == PSA_SUCCESS && salt != NULL && salt_len > 0) {
        status = psa_hash_update(&op, salt, salt_len);
    }
    if (status == PSA_SUCCESS) {
        status = psa_hash_update(&op, (const unsigned char *)(password ? password : ""),
                                 password ? strlen(password) : 0);
    }
    if (status == PSA_SUCCESS) {
        status = psa_hash_finish(&op, out_hash, 32, &hash_len);
    }
    if (status != PSA_SUCCESS) {
        memset(out_hash, 0, 32);
    }
}

SemaphoreHandle_t app_mutex_ensure(SemaphoreHandle_t *handle,
                                   StaticSemaphore_t *buffer,
                                   portMUX_TYPE *mux,
                                   bool recursive)
{
    SemaphoreHandle_t h;

    portENTER_CRITICAL(mux);
    h = *handle;
    if (h == NULL) {
        h = recursive ? xSemaphoreCreateRecursiveMutexStatic(buffer)
                      : xSemaphoreCreateMutexStatic(buffer);
        *handle = h;
    }
    portEXIT_CRITICAL(mux);
    return h;
}

EventGroupHandle_t app_event_group_ensure(EventGroupHandle_t *handle,
                                          StaticEventGroup_t *buffer,
                                          portMUX_TYPE *mux)
{
    EventGroupHandle_t h;

    portENTER_CRITICAL(mux);
    h = *handle;
    if (h == NULL) {
        h = xEventGroupCreateStatic(buffer);
        *handle = h;
    }
    portEXIT_CRITICAL(mux);
    return h;
}

void app_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t index = 0;

    if (dst_size == 0) {
        return;
    }

    while (src[index] != '\0' && index + 1 < dst_size) {
        dst[index] = src[index];
        index++;
    }
    dst[index] = '\0';
}

void app_copy_fixed_bytes(uint8_t *dst, size_t dst_size, const char *src)
{
    size_t len;

    if (dst_size == 0) {
        return;
    }
    memset(dst, 0, dst_size);
    if (src == NULL) {
        return;
    }
    len = strlen(src);
    if (len > dst_size) {
        len = dst_size;
    }
    if (len > 0) {
        memcpy(dst, src, len);
    }
}

void app_json_escape_string(char *dst, size_t dst_size, const char *src)
{
    size_t index = 0;

    if (dst_size == 0) {
        return;
    }

    while (*src != '\0' && index + 1 < dst_size) {
        const char *replacement = NULL;
        char unicode_escape[8];
        char ch = *src++;

        switch (ch) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
            break;
        case '/':
            replacement = "\\/";
            break;
        case '\b':
            replacement = "\\b";
            break;
        case '\f':
            replacement = "\\f";
            break;
        case '\n':
            replacement = "\\n";
            break;
        case '\r':
            replacement = "\\r";
            break;
        case '\t':
            replacement = "\\t";
            break;
        default:
            /* 其余 C0 控制字符按 RFC 8259 用 \uXXXX 转义,不再丢字替换为 '?' */
            if ((unsigned char)ch < 0x20) {
                snprintf(unicode_escape, sizeof(unicode_escape),
                         "\\u%04X", (unsigned)(unsigned char)ch);
                replacement = unicode_escape;
            }
            break;
        }

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

const char *app_bt_mode_to_string(uint8_t mode)
{
    switch (mode) {
    case BT_MODE_BLE:
        return "ble";
    case BT_MODE_SPP:
        return "spp";
    default:
        return "off";
    }
}

uint8_t app_bt_mode_from_string(const char *mode)
{
    if (strcmp(mode, "ble") == 0) {
        return BT_MODE_BLE;
    }
    if (strcmp(mode, "spp") == 0) {
        return BT_MODE_SPP;
    }
    return BT_MODE_OFF;
}

void app_format_hex_bytes(const uint8_t *bytes, size_t len,
                          char *buffer, size_t buffer_size,
                          char separator)
{
    size_t index;
    size_t offset = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (bytes == NULL || len == 0) {
        return;
    }

    for (index = 0; index < len; index++) {
        const char *fmt;
        int written;
        size_t min_left = (separator != '\0') ? 4 : 3;

        if (offset + min_left > buffer_size) {
            break;
        }
        if (separator != '\0' && index > 0) {
            buffer[offset++] = separator;
        }
        fmt = "%02X";
        written = snprintf(buffer + offset, buffer_size - offset,
                           fmt, (unsigned)bytes[index]);
        if (written < 0 || (size_t)written >= buffer_size - offset) {
            break;
        }
        offset += (size_t)written;
    }
}
