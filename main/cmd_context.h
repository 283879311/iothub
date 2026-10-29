#pragma once

#include <stdbool.h>
#include <stdint.h>

/* ============================================================================
 * 命令来源上下文(C1 阶段 1:可信来源,设计见 docs/C1_cmd_context阶段1设计_*.md)
 *
 * 四个字段各司其职,不得混用:
 *   source   —— 本请求直接从哪个入口进入;内部派生执行为 INTERNAL;
 *   origin   —— 因果链最初入口,内部任务不得改写;是追踪信息,不是授权凭据;
 *   exec_env —— 当前代码运行上下文(入口任务 / 内部派生);
 *   auth_level —— 主体认证等级,不从 source 推导(阶段 1 信息性,阶段 2 消费)。
 *
 * INTERNAL 不自动获得特权:它只是"这段代码不是入口"的标记,阶段 1 的权限
 * 行为与改造前逐字节一致。
 * ========================================================================== */

/* 命令入口来源。UNKNOWN=0:零值默认拒绝 —— 未初始化/未注入的上下文一律不可信。
 * 数值为纯 RAM 运行时用,不落 NVS、不进 BLE/MQTT 线格式;重编号经全仓调用点
 * 核对(4 个 execute 调用点全部显式传值,s_net_* 先 memset 后在读取前赋值)。 */
typedef enum {
    APP_CMD_SOURCE_UNKNOWN  = 0,
    APP_CMD_SOURCE_MQTT     = 1,
    APP_CMD_SOURCE_BLE      = 2,
    APP_CMD_SOURCE_HTTP     = 3,   /* HTTP 服务已删,枚举保留(语义仍在) */
    APP_CMD_SOURCE_TOUCH    = 4,
    APP_CMD_SOURCE_SERIAL   = 5,   /* 预留:UART 维护通道承载未定,不接线 */
    APP_CMD_SOURCE_INTERNAL = 6,   /* 内部派生执行;不作入口值 */
} app_cmd_source_t;

typedef enum {
    APP_CMD_EXEC_ENTRY = 0,        /* 入口任务直接执行 */
    APP_CMD_EXEC_INTERNAL,         /* 内部派生(如 async apply 的 trial 提交) */
} app_cmd_exec_env_t;

typedef enum {
    APP_CMD_AUTH_NONE = 0,
    APP_CMD_AUTH_LOCAL_TOKEN,      /* transport 层已验 token(BLE login / MQTT broker) */
} app_cmd_auth_level_t;

/* 派生深度上限:防止内部命令递归成环。entry=0,一层派生=1。 */
#define APP_CMD_CTX_DEPTH_MAX 2

typedef struct {
    app_cmd_source_t source;
    app_cmd_source_t origin;
    app_cmd_exec_env_t exec_env;
    app_cmd_auth_level_t auth_level;
    uint8_t depth;
    uint32_t trace_id;             /* 日志关联;派生继承父值(链路关联) */
    uint32_t channel_epoch;        /* 阶段 1 恒 0 占位;阶段 3 接通道代数 */
} app_cmd_context_t;

/* 上下文合法性(评审 R1 收紧后):
 *   - source=UNKNOWN / auth_level 越界 / channel_epoch!=0 → 一律拒绝
 *     (epoch 恒 0 是阶段 1 占位契约,阶段 3 放宽并加真实校验);
 *   - ENTRY:source 属于入口集合 MQTT..SERIAL(1..5)、source==origin、depth=0;
 *   - INTERNAL:source 恰为 INTERNAL、origin 属于入口集合、1<=depth<=上限;
 *   - exec_env 为其它任何值 → 拒绝。 */
bool app_cmd_ctx_valid(const app_cmd_context_t *ctx);

/* 入口构造:source==origin、ENTRY、depth=0、trace 自增。
 * source 传 UNKNOWN/INTERNAL 时构造出的上下文不合法,执行层会拒绝。 */
app_cmd_context_t app_cmd_ctx_entry(app_cmd_source_t source);

/* 内部派生构造:source=INTERNAL、origin 继承、depth+1、trace 继承。
 * 父上下文不合法或 depth 已达上限时返回 false(拒绝继续派生)。 */
bool app_cmd_ctx_internal_child(const app_cmd_context_t *parent,
                                app_cmd_context_t *out);
