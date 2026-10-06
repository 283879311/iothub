#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

void app_copy_string(char *dst, size_t dst_size, const char *src);
void app_json_escape_string(char *dst, size_t dst_size, const char *src);
void app_copy_fixed_bytes(uint8_t *dst, size_t dst_size, const char *src);
void app_format_hex_bytes(const uint8_t *bytes, size_t len,
                          char *buffer, size_t buffer_size,
                          char separator);
const char *app_bt_mode_to_string(uint8_t mode);
uint8_t app_bt_mode_from_string(const char *mode);

/* 并发安全的锁懒初始化:临界区内只做无堆分配的静态创建,
 * 消除"首次并发调用创建两把锁"的竞态,且创建不会失败。 */
SemaphoreHandle_t app_mutex_ensure(SemaphoreHandle_t *handle,
                                   StaticSemaphore_t *buffer,
                                   portMUX_TYPE *mux,
                                   bool recursive);
EventGroupHandle_t app_event_group_ensure(EventGroupHandle_t *handle,
                                          StaticEventGroup_t *buffer,
                                          portMUX_TYPE *mux);

/* Web 登录口令哈希:SHA-256(salt || password),盐 16 字节,输出 32 字节 */
void app_web_hash_password(const uint8_t *salt, size_t salt_len,
                           const char *password, uint8_t *out_hash);
