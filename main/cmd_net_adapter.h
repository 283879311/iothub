#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "cmd_context.h"

/* Trusted protocol-adapter entry points for network transactions.
 * These are process-internal APIs: callers must pass a context they built
 * themselves, never one derived from client JSON. */

bool app_cmd_net_tryconnect_adapter(const app_cmd_context_t *ctx,
                                    const cJSON *params,
                                    cJSON **resp_out, const char **err_out);
bool app_cmd_net_apply_adapter(const app_cmd_context_t *ctx,
                               const cJSON *params,
                               cJSON **resp_out, const char **err_out);
bool app_cmd_net_cancel_adapter(const app_cmd_context_t *ctx,
                                const cJSON *params,
                                cJSON **resp_out, const char **err_out);
