/* 宿主单测:验证 main/json_escape.c 的"分片流式转义"与"整段转义"**逐字节等价**。
 *
 * 运行(gcc,不依赖 ESP-IDF):
 *   gcc -I main -o /tmp/t_json_escape \
 *       tools/json_escape/test_json_escape_stream.c main/json_escape.c && /tmp/t_json_escape
 *   期望:结果: 共 N 例, 失败 0
 *
 * 为什么重要:/api/v1/uart/console 由"整段转义 + 一次性发送"改为"分片转义 + chunked 发送"
 * (见 问题与版本优化.md 第 45 项),必须证明输出与旧路径完全一致,否则前端解析行为会变。 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "json_escape.h"

static int s_total;
static int s_failed;

/* 被测路径:按 window 大小的分片反复步进,把结果拼接起来(模拟 httpd_resp_send_chunk 流) */
static size_t stream_escape(const char *src, size_t window, char *dst, size_t dst_size)
{
    char *out = (char *)malloc(window);
    size_t cursor = 0;
    size_t total = 0;
    size_t guard = 0;
    size_t limit = strlen(src) * 8 + 32;
    bool done = false;

    if (out == NULL) {
        return (size_t)-1;
    }

    while (!done) {
        size_t n = 0;

        done = app_json_escape_stream_step(src, &cursor, out, window, &n);
        if (total + n + 1 > dst_size) {          /* 缓冲区不够:测试用例本身写错了 */
            free(out);
            return (size_t)-1;
        }
        memcpy(dst + total, out, n);
        total += n;

        if (++guard > limit) {                   /* 死循环保护(步进无法推进) */
            free(out);
            return (size_t)-1;
        }
    }

    dst[total] = '\0';
    free(out);
    return total;
}

static void check_case(const char *name, const char *src)
{
    /* 最长转义原子为 6 字节(\u00XX),故参考缓冲按 6 倍 + NUL 准备,避免截断干扰对比 */
    static const size_t windows[] = { 6, 7, 8, 13, 64, 512, 4096 };
    size_t src_len = strlen(src);
    size_t buf_size = src_len * 6 + 8;
    char *ref = (char *)malloc(buf_size);
    char *got = (char *)malloc(buf_size);
    size_t w;

    if (ref == NULL || got == NULL) {
        printf("  [FAIL] %s (malloc)\n", name);
        s_failed++;
        free(ref);
        free(got);
        return;
    }

    app_json_escape_string(ref, src_len * 6 + 1, src);

    for (w = 0; w < sizeof(windows) / sizeof(windows[0]); w++) {
        size_t n = stream_escape(src, windows[w], got, buf_size);

        s_total++;
        if (n == (size_t)-1 || strcmp(got, ref) != 0) {
            s_failed++;
            printf("  [FAIL] %s (window=%u)\n", name, (unsigned)windows[w]);
            printf("         ref[%u]=[%s]\n", (unsigned)strlen(ref), ref);
            printf("         got[%u]=[%s]\n", (unsigned)(n == (size_t)-1 ? 0 : n),
                   (n == (size_t)-1) ? "<no-progress>" : got);
        }
    }

    free(ref);
    free(got);
}

int main(void)
{
    char c0_all[0x20];      /* 0x01..0x1F 全量 C0 控制字符 */
    char quotey[201];       /* 全引号:每字节膨胀为 2 字节,覆盖片边界 */
    char utf8[] = "\xE4\xBD\xA0\xE5\xA5\xBD\xF0\x9F\x9A\x80";   /* 你好 + emoji(4 字节序列) */
    char console_plain[] = "AT+CMD\r\n\x02\x03 raw \\x04 tail\n";
    char console_hex[] = "41 54 20 2B 43 4D 44 0D 0A ";
    char long_hex[3074];
    size_t i;

    for (i = 0; i < sizeof(c0_all) - 1; i++) {
        c0_all[i] = (char)(i + 1);
    }
    c0_all[sizeof(c0_all) - 1] = '\0';

    memset(quotey, '"', sizeof(quotey) - 1);
    quotey[sizeof(quotey) - 1] = '\0';

    for (i = 0; i < sizeof(long_hex) - 1; i++) {
        long_hex[i] = "0123456789ABCDEF "[i % 17];
    }
    long_hex[sizeof(long_hex) - 1] = '\0';

    printf("json_escape 分片流式 == 整段转义 等价性单测\n");

    check_case("empty", "");
    check_case("ascii-plain", "hello world");
    check_case("all-quotes", quotey);
    check_case("c0-controls", c0_all);
    check_case("escapes-set", "\\ / \b \f \n \r \t end");
    check_case("del-and-high", "\x7F\x80\xFF tail");
    check_case("utf8", utf8);
    check_case("console-plain-like", console_plain);
    check_case("console-hex-like", console_hex);
    check_case("long-hex-3073", long_hex);

    printf("结果: 共 %d 例, 失败 %d\n", s_total, s_failed);
    return s_failed == 0 ? 0 : 1;
}
