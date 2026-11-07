#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Stage 0 config-transaction gate. State is protected by app_config_lock
 * (recursive); no separate long-held lock is introduced. */

typedef struct {
    uint32_t token;
} app_cmd_cfg_txn_t;

/* Claim a network transaction. Returns false if one is already active. */
bool app_cmd_cfg_txn_begin(app_cmd_cfg_txn_t *out);

/* Release only if the active token matches. Idempotent for stale tokens. */
bool app_cmd_cfg_txn_end(app_cmd_cfg_txn_t txn);

/* Enter the config-write gate. On true the caller holds app_config_lock and
 * must call guard_exit() after the write. On false the lock is already
 * released and the caller must return busy.
 *
 * Long-term constraint: while the gate is held, do not call Wi-Fi/MQTT APIs,
 * wait on the network, or invoke callbacks that may re-enter config. NVS stays
 * on the existing app_config_lock path; no new long-held lock is introduced. */
bool app_cmd_cfg_txn_guard_enter(void);
void app_cmd_cfg_txn_guard_exit(void);

bool app_cmd_cfg_txn_active(void);
