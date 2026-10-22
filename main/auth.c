#include <stdint.h>
#include <string.h>

#include "auth.h"
#include "common.h"
#include "config.h"

extern app_config_t s_config;

bool app_auth_check_credentials(const char *username, const char *password)
{
    char stored_username[33];
    uint8_t salt[16];
    uint8_t stored[32];
    uint8_t calc[32];
    uint8_t diff = 0;
    size_t i;

    if (username == NULL || password == NULL || username[0] == '\0') {
        return false;
    }
    app_config_lock();
    app_copy_string(stored_username, sizeof(stored_username), s_config.web_username);
    memcpy(salt, s_config.web_pass_salt, sizeof(salt));
    memcpy(stored, s_config.web_pass_hash, sizeof(stored));
    app_config_unlock();

    if (stored_username[0] == '\0' || strcmp(username, stored_username) != 0) {
        return false;
    }
    app_web_hash_password(salt, sizeof(salt), password, calc);
    for (i = 0; i < sizeof(calc); i++) {
        diff |= (uint8_t)(calc[i] ^ stored[i]);
    }
    return diff == 0;
}
