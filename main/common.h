#pragma once

#include <stddef.h>
#include <stdint.h>

void app_copy_string(char *dst, size_t dst_size, const char *src);
void app_json_escape_string(char *dst, size_t dst_size, const char *src);
void app_copy_fixed_bytes(uint8_t *dst, size_t dst_size, const char *src);
void app_format_hex_bytes(const uint8_t *bytes, size_t len,
                          char *buffer, size_t buffer_size,
                          char separator);
const char *app_bt_mode_to_string(uint8_t mode);
uint8_t app_bt_mode_from_string(const char *mode);
