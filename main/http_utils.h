#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

esp_err_t app_http_send_json_text(httpd_req_t *req, const char *status, const char *payload);
esp_err_t app_http_send_jsonf(httpd_req_t *req, const char *status, const char *fmt, ...);
bool app_json_find_string(const char *json, const char *key, char *out, size_t out_size);
bool app_json_find_bool(const char *json, const char *key, bool *value);
bool app_json_find_u16(const char *json, const char *key, uint16_t *value);
bool app_json_find_u32(const char *json, const char *key, uint32_t *value);
bool app_json_find_double(const char *json, const char *key, double *value);
