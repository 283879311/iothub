#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "cmd.h"
#include "cmd_context.h"

/* 2026-11-05:v0 协议已彻底删除,不再有 APP_CMD_V0_COMPAT 回退开关。 */

/* Unified v1 command registry. Transports keep only transport concerns
 * (framing, auth, rate limit); command policy and dispatch live here. */

bool app_cmd_v1_allowed(const app_cmd_context_t *ctx, const char *cmd);

/* Execute one v1 command. On success *result owns a JSON object and *out is
 * filled for transport post-actions (e.g. reboot_after_response). On failure
 * *err is a static v0 error string; the transport maps it to a v1 code. */
bool app_cmd_v1_execute(const app_cmd_context_t *ctx, const char *cmd,
                        const cJSON *args, cJSON **result,
                        app_cmd_result_t *out, const char **err);

/* Test/introspection helper: 表内命令返回其 canonical 名,表外返回 NULL。 */
const char *app_cmd_v1_internal_method(const char *cmd);
