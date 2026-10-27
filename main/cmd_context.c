#include <stddef.h>

#include "cmd_context.h"

/* ============================================================================
 * 命令来源上下文实现。刻意只依赖自身头文件(不碰 cJSON/ESP 头),
 * host 单测(tests/cmd_context)可直接编译真实源码 —— 与 auth_token.c 同一先例。
 * ========================================================================== */

static uint32_t s_trace_seq;   /* 尽力而为的日志关联计数,不持久、不作安全值 */

bool app_cmd_ctx_valid(const app_cmd_context_t *ctx)
{
    if (ctx == NULL || ctx->source == APP_CMD_SOURCE_UNKNOWN) {
        return false;
    }
    if (ctx->exec_env == APP_CMD_EXEC_ENTRY) {
        return ctx->source == ctx->origin &&
               ctx->source != APP_CMD_SOURCE_INTERNAL &&
               ctx->depth == 0;
    }
    return ctx->exec_env == APP_CMD_EXEC_INTERNAL &&
           ctx->origin != APP_CMD_SOURCE_UNKNOWN &&
           ctx->origin != APP_CMD_SOURCE_INTERNAL &&
           ctx->depth >= 1 && ctx->depth <= APP_CMD_CTX_DEPTH_MAX;
}

app_cmd_context_t app_cmd_ctx_entry(app_cmd_source_t source)
{
    app_cmd_context_t ctx = {0};

    ctx.source = source;
    ctx.origin = source;
    ctx.exec_env = APP_CMD_EXEC_ENTRY;
    /* 阶段 1 占位:transport 迁移显式 ctx_entry 时由各入口填写;
     * 本阶段无读取者,恒为 NONE 不改变任何行为。 */
    ctx.auth_level = APP_CMD_AUTH_NONE;
    ctx.depth = 0;
    ctx.trace_id = ++s_trace_seq;
    ctx.channel_epoch = 0;
    return ctx;
}

bool app_cmd_ctx_internal_child(const app_cmd_context_t *parent,
                                app_cmd_context_t *out)
{
    if (out == NULL || !app_cmd_ctx_valid(parent) ||
        parent->depth >= APP_CMD_CTX_DEPTH_MAX) {
        return false;
    }
    *out = *parent;
    out->source = APP_CMD_SOURCE_INTERNAL;
    out->exec_env = APP_CMD_EXEC_INTERNAL;
    out->depth = (uint8_t)(parent->depth + 1);
    /* origin/trace_id/channel_epoch/auth_level 继承父值 */
    return true;
}
