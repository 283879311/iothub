#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

bool app_json_find_string(const char *json, const char *key, char *out, size_t out_size);
bool app_json_find_bool(const char *json, const char *key, bool *value);
bool app_json_find_u16(const char *json, const char *key, uint16_t *value);
bool app_json_find_u32(const char *json, const char *key, uint32_t *value);
