#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "cmd.h"
#include "cmd_context.h"
#include "cmd_v1_id.h"

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

/* v1 请求 id 的 cJSON 适配(PROTO-ID-01),BLE/MQTT 共用。
 * from_root:先 reset out;有效 string/number 返回 true;缺失、类型不支持、
 * 超长、空串、非法 UTF-8、非有限 number 返回 false 且 out 保持 NONE。
 * 本函数不做 \u0000 扫描;调用方必须先对原始 payload 扫描。 */
bool app_cmd_v1_id_from_root(const cJSON *root, app_cmd_v1_id_t *out);

/* 把 id 写入响应对象:STRING/NUMBER 按原类型;NONE 为成功的 no-op。
 * id 为 NULL、未知 kind 或 cJSON 添加失败返回 false。 */
bool app_cmd_v1_id_add(cJSON *root, const app_cmd_v1_id_t *id);
