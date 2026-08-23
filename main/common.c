#include <stdio.h>
#include <string.h>

#include "constants.h"
#include "common.h"

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
        char ch = *src++;

        switch (ch) {
        case '\\':
            replacement = "\\\\";
            break;
        case '"':
            replacement = "\\\"";
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
            break;
        }

        if (replacement != NULL) {
            while (*replacement != '\0' && index + 1 < dst_size) {
                dst[index++] = *replacement++;
            }
            continue;
        }

        if ((unsigned char)ch < 32) {
            dst[index++] = '?';
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
