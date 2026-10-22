#pragma once

#include <stdbool.h>

/* BLE 登录凭据校验。Web/HTTP 已删除，但本地管理仍复用同一份
 * web_username / salt+SHA256 口令哈希。 */
bool app_auth_check_credentials(const char *username, const char *password);
