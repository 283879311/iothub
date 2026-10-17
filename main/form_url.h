#pragma once

/* 表单(urlencoded)与 Cookie 头的**纯文本解析** —— 零 ESP-IDF 依赖,
 * 便于 tools/form_url/ 下的宿主单测直接编译同一份源码(与 main/json_escape.c 同约定)。
 *
 * 用途:强制门户登录表单(POST /api/v1/auth/login)与 Web 会话 Cookie 的解析
 *       (见 1.3版本优化.md 1.3-01)。这两段是最容易在真机上"偶发解析错"的逻辑,
 *       故独立成文件并配宿主单测。 */

#include <stdbool.h>
#include <stddef.h>

/* urlencoded 解码:就地处理 '+' → 空格、%XX → 字节(非法/不足两位的转义原样保留) */
void app_url_decode(char *s);

/* 就地解码整个表单体(逐字段只解码 '=' 之后的部分,不动键名)。
 * ⚠️ 与 `app_form_lookup` 连用时**无法处理"值里含 &"**:本函数会把 `%26` 解码成裸 '&',
 * 之后按 '&' 切字段即把该值截断。新代码请直接用 `app_form_lookup_raw`(先切后解码)。 */
void app_form_decode_all(char *body);

/* 取字段(**推荐入口**):在**原始**表单体上先切字段,再只对取出的值做 urlencoded 解码。
 * 与 `app_form_decode_all` + `app_form_lookup` 的组合相比,本函数能正确处理
 * "值里含 &(编码为 %26)"、"+ 表示空格" 等情况 */
bool app_form_lookup_raw(const char *body, const char *key, char *out, size_t out_size);

/* 取字段(须先 app_form_decode_all);out 仅在命中时写入;值为空串也算命中 */
bool app_form_lookup(const char *body, const char *key, char *out, size_t out_size);

/* 从 Cookie 头取值:键名须位于串首或 "; " 之后且紧跟 '='(避免误匹配子串);
 * out 仅在命中且值非空时写入并返回 true */
bool app_cookie_value(const char *cookie_header, const char *name,
                      char *out, size_t out_size);
