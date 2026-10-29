#include <stddef.h>
#include <stdatomic.h>

#include "cmd_context.h"

/* ============================================================================
 * 命令来源上下文实现。刻意只依赖自身头文件 + C11 标准库(不碰 cJSON/ESP 头),
 * host 单测(tests/cmd_context)可直接编译真实源码 —— 与 auth_token.c 同一先例。
 * ========================================================================== */

/* 原子自增(评审 R1-P2):ctx_entry 可从多个任务并发调用(BLE/MQTT dispatcher、
 * touch),普通 ++ 是数据竞争(UB),不只是"可能重号"。relaxed 序足够 ——
 * 只做日志关联,不参与任何同步。32 位会回绕,消费方不得假定 trace 唯一。 */
static _Atomic uint32_t s_trace_seq;

/* 合法入口来源集合:MQTT..SERIAL(1..5)。
 * UNKNOWN(0) 与 INTERNAL(6) 及一切未定义值(如 99)都不是入口。
 * HTTP/SERIAL 当前无接线入口,但枚举语义保留,接入时无需改本函数。 */
static bool app_cmd_src_is_entry(app_cmd_source_t s)
{
    return (unsigned)s >= (unsigned)APP_CMD_SOURCE_MQTT &&
           (unsigned)s <= (unsigned)APP_CMD_SOURCE_SERIAL;
}

bool app_cmd_ctx_valid(const app_cmd_context_t *ctx)
{
    if (ctx == NULL || ctx->source == APP_CMD_SOURCE_UNKNOWN) {
        return false;
    }
    /* 结构性不变量:占位字段越界同样整包拒绝(fail-loud)。
     * epoch 恒 0 是阶段 1 占位契约,阶段 3 接通道代数时同步放宽本判据。 */
    if ((unsigned)ctx->auth_level > (unsigned)APP_CMD_AUTH_LOCAL_TOKEN ||
        ctx->channel_epoch != 0) {
        return false;
    }
    if (ctx->exec_env == APP_CMD_EXEC_ENTRY) {
        return app_cmd_src_is_entry(ctx->source) &&
               ctx->source == ctx->origin &&
               ctx->depth == 0;
    }
    /* 内部派生:source 必须恰为 INTERNAL(防"内部环境冒充入口来源"),
     * origin 必须是合法入口;exec_env 非法值落到这里天然被拒。 */
    return ctx->exec_env == APP_CMD_EXEC_INTERNAL &&
           ctx->source == APP_CMD_SOURCE_INTERNAL &&
           app_cmd_src_is_entry(ctx->origin) &&
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
    ctx.trace_id = atomic_fetch_add_explicit(&s_trace_seq, 1,
                                             memory_order_relaxed) + 1;
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
