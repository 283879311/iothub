#pragma once

#include <stdbool.h>

#include "cJSON.h"
#include "cmd_v1_id.h"

/* v1 请求 id 的 cJSON 解析/写出适配(PROTO-ID-01)。
 *
 * 独立成文件的原因：cmd_v1_id.* 刻意不依赖 cJSON/ESP-IDF，便于纯 C host 测试；
 * 而解析/写出路径必须覆盖 cJSON 的行为（M8），只能在带 cJSON 的环境测。
 * 因此把这两个适配拆到这里，host 用例可以脱离命令内核单独编译
 * （见 tests/proto_id/test_cmd_v1_id_json.c）。
 *
 * from_root：先 reset；有效 string/number 返回 true；缺失、类型不支持、超长、
 *   空串、非法 UTF-8、非有限 number 返回 false 且 out 保持 NONE。
 *   不做 \u0000 扫描——调用方必须先对原始 payload 扫描。
 * add：STRING 原样写入；NUMBER 以 "%.17g" 文本作为 JSON number 写入（绕过
 *   cJSON 的 "%1.15g + compare_double" 判定，见 M8）；NONE 为成功的 no-op。
 *   id 为 NULL、未知 kind 或 cJSON 添加失败返回 false。 */
bool app_cmd_v1_id_from_root(const cJSON *root, app_cmd_v1_id_t *out);
bool app_cmd_v1_id_add(cJSON *root, const app_cmd_v1_id_t *id);
