#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* BLE 登录凭据校验。Web/HTTP 已删除，但本地管理仍复用同一份
 * web_username / salt+SHA256 口令哈希。 */
bool app_auth_check_credentials(const char *username, const char *password);

/* ============================================================================
 * 连接级短时 token 存储(按来源分槽)
 *
 * 背景:token 原先由 ble_cmd.c 自己持有单一全局槽位(字符串 + 过期时间),
 * 签发/校验/断连清理三处无锁访问同一对变量 —— GATT 回调与命令任务并发时
 * 可能读到"新 token + 旧过期时间"的撕裂组合;串口维护通道接入后还需要同一套
 * 语义的第二个来源。本模块把"槽位 + TTL + 常量时间比较"收在一处,所有访问
 * 在同一把临界区锁内完成。
 *
 * 实现文件偏差(A 阶段评审确认,2026-10-26):原实施标准要求不新增源文件;
 * 实际实现为独立的 auth_token.c,理由是 host 单测可直接编译真实源码,不必
 * 为 auth.c 的 config/common 依赖伪造整套配置结构体。CMakeLists 仅增加一行,
 * 该偏差已书面接受;后续同类偏差须先报批,不得由实施方单方面改写标准。
 *
 * 设计约束(A 阶段契约,改动前先对齐):
 *   - 临界区内只做:内存拷贝、时间比较、32 字节异或循环;
 *     随机数、十六进制编码、日志、cJSON 一律锁外;
 *   - 比较必须定长 32 次异或,不用 memcmp 的短路版本;
 *   - issue 的响应取自局部缓冲,禁止二次读槽位回包;
 *   - 过期判断用 now_ms >= expire_ms(到点即失效)。
 * ========================================================================== */

#define APP_TOKEN_HEX_LEN 32
#define APP_TOKEN_TTL_MS  600000 /* 10 分钟,原 BLE token TTL 迁入 */

typedef enum {
    TOKEN_SRC_BLE = 0,
    TOKEN_SRC_SERIAL, /* 槽位已就绪但尚未接入(UART 维护通道属 B/C 阶段) */
    TOKEN_SRC_MAX,
} token_source_t;

/* 签发:锁外生成随机数并编码,锁内写入槽位。
 * 成功返回 true,且 out 保证 NUL 结尾;out_size < 33 或 src 越界返回 false
 * (返回 false 时槽位保持原值,不破坏已签发的旧 token)。 */
bool app_token_store_issue(token_source_t src, char *out, size_t out_size);

/* 校验:格式检查(32 位十六进制)在锁外,32 字节定长异或在锁内完成。
 * 过期则在锁内顺手清槽。token 为 NULL/空串/长度不对/非十六进制一律 false。 */
bool app_token_store_validate(token_source_t src, const char *token);

void app_token_store_clear(token_source_t src); /* 只清指定来源 */
/* 清全部来源。改密/全局策略用；必须遍历 TOKEN_SRC_MAX，不得退化为只清 BLE。 */
void app_token_store_clear_all(void);
