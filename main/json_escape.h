#pragma once

/* JSON 字符串转义 —— **单一实现来源**,且无 ESP-IDF 依赖
 * (刻意保持"零依赖",以便 tools/json_escape/ 下的宿主单测直接编译**同一份源码**,
 *  而不是维护一份参考副本,避免两条实现漂移)。
 *
 * 两个出口共用同一张转义表(见 问题与版本优化.md 第 45 项):
 *   ① app_json_escape_string()      —— 整段转义(历史接口,调用点不变)
 *   ② app_json_escape_stream_step() —— 分片流式转义(/api/v1/uart/console 零堆分配改造)
 */

#include <stdbool.h>
#include <stddef.h>

/* 单字符转义映射(RFC 8259):
 *   返回需要写入的转义序列(如 \" \\ \n 或 \u00XX);无需转义时返回 NULL(原样输出该字节)。
 *   buf 仅用于现场格式化的 \uXXXX 序列,调用方须保证 >= 7 字节。 */
const char *app_json_escape_seq(char ch, char *buf);

/* 整段转义:迁自 common.c,语义**逐字节不变**(dst 不足时按转义原子边界截断并 NUL 收尾) */
void app_json_escape_string(char *dst, size_t dst_size, const char *src);

/* 分片流式转义(可续写,永不写出半个转义原子):
 *   从 *cursor 处继续把 src 的转义结果写入 out(容量 out_size);
 *   实际写入长度回写 *out_len;返回 src 是否已全部处理完。典型用法:
 *
 *     size_t cursor = 0;
 *     do {
 *         size_t n = 0;
 *         bool done = app_json_escape_stream_step(src, &cursor, out, sizeof(out), &n);
 *         if (n > 0) { send_chunk(out, n); }
 *     } while (!done);
 *
 *   约定:out_size >= 6(最长转义原子 \u00XX 为 6 字节),否则可能无法推进;
 *   out_size == 0 / src == NULL / out == NULL 时返回 false 且不推进。 */
bool app_json_escape_stream_step(const char *src, size_t *cursor,
                                 char *out, size_t out_size, size_t *out_len);
