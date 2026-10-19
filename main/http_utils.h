#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_http_server.h"

esp_err_t app_http_send_json_text(httpd_req_t *req, const char *status, const char *payload);
esp_err_t app_http_send_jsonf(httpd_req_t *req, const char *status, const char *fmt, ...);
/* 响应前关 Nagle(分块/多段响应提速,见 问题与版本优化.md 1.3-36) */
void      app_http_tcp_nodelay(httpd_req_t *req);

/* ---- 零堆 JSON 构建基建(问题与版本优化.md 1.3-12)----------------------------------
 * 背景:原 status/network 两个热接口走 cJSON"建树 + 序列化"(两份堆),在
 * 并发 + 堆偏紧(BLE 常驻时 free 仅 ~16KB)时会 500 或出现秒级延迟尖峰。
 *
 * 本组接口把响应构造收敛为"静态缓冲 + 互斥锁":
 *   - app_json_build_lock()/unlock():构建期互斥(可重入,与 send 内部同一把锁)。
 *     必须在锁内完成"取转义串 + 拼接 + 发送",否则并发请求会互相覆盖。
 *   - app_json_escaped(src):把字符串转义进**轮转静态槽**并返回指针,供 send_jsonf 的
 *     格式参数直接使用(零栈开销)。每次加锁期间最多用 8 个槽,超出会从头覆盖。
 *   - app_http_send_jsonf():内部已自持同一把锁(可重入),故未做转义的老调用点
 *     自动获得并发保护。----------------------------------------------------------------- */
void app_json_build_lock(void);
void app_json_build_unlock(void);
const char *app_json_escaped(const char *src);
bool app_json_find_string(const char *json, const char *key, char *out, size_t out_size);
bool app_json_find_bool(const char *json, const char *key, bool *value);
bool app_json_find_u16(const char *json, const char *key, uint16_t *value);
bool app_json_find_u32(const char *json, const char *key, uint32_t *value);
bool app_json_find_double(const char *json, const char *key, double *value);
