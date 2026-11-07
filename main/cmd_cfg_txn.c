#include <stddef.h>

#include "cmd_cfg_txn.h"

/* Implemented by config.c. Kept as externs so this module stays host-testable. */
void app_config_lock(void);
void app_config_unlock(void);

static bool s_active;
static uint32_t s_token;
static uint32_t s_next_token;

bool app_cmd_cfg_txn_begin(app_cmd_cfg_txn_t *out)
{
    bool ok = false;

    if (out == NULL) {
        return false;
    }
    app_config_lock();
    if (!s_active) {
        uint32_t next = s_next_token + 1;

        if (next == 0) {
            next = 1;
        }
        s_next_token = next;
        s_token = next;
        s_active = true;
        out->token = next;
        ok = true;
    }
    app_config_unlock();
    return ok;
}

bool app_cmd_cfg_txn_end(app_cmd_cfg_txn_t txn)
{
    bool released = false;

    app_config_lock();
    if (s_active && txn.token != 0 && txn.token == s_token) {
        s_active = false;
        s_token = 0;
        released = true;
    }
    app_config_unlock();
    return released;
}

bool app_cmd_cfg_txn_guard_enter(void)
{
    app_config_lock();
    if (s_active) {
        app_config_unlock();
        return false;
    }
    return true;
}

void app_cmd_cfg_txn_guard_exit(void)
{
    app_config_unlock();
}

bool app_cmd_cfg_txn_active(void)
{
    bool active;

    app_config_lock();
    active = s_active;
    app_config_unlock();
    return active;
}
