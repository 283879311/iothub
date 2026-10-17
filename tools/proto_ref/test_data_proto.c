/*
 * data_proto 主机端单测:用《快递秤通信协议汇总》里的真实报文逐条验证解析结果。
 *
 * ⚠️ 本目录**不参与固件构建**(见 问题与版本优化.md 第 36 项):设备端只做字节透传,
 * 协议解析放在手机 / 电脑 / 服务器侧。这里保留的是**已用真实报文验证过的参考实现**,
 * 供端上移植或直接复用。
 *
 * 解析器为纯函数(无 ESP-IDF 依赖),可直接用 gcc 编译运行:
 *   gcc -I tools/proto_ref -o /tmp/t \
 *       tools/proto_ref/test_data_proto.c tools/proto_ref/data_proto.c -lm && /tmp/t
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_proto.h"

static int g_checks;
static int g_fails;

static size_t hex2bin(const char *hex, uint8_t *out, size_t cap)
{
    size_t n = 0;
    int hi = -1;
    const char *p;

    for (p = hex; *p != '\0'; p++) {
        int v;
        if (*p == ' ') {
            continue;
        }
        if (*p >= '0' && *p <= '9') {
            v = *p - '0';
        } else if (*p >= 'A' && *p <= 'F') {
            v = *p - 'A' + 10;
        } else if (*p >= 'a' && *p <= 'f') {
            v = *p - 'a' + 10;
        } else {
            fprintf(stderr, "bad hex char '%c'\n", *p);
            exit(2);
        }
        if (hi < 0) {
            hi = v;
        } else {
            if (n < cap) {
                out[n++] = (uint8_t)((hi << 4) | v);
            }
            hi = -1;
        }
    }
    return n;
}

typedef struct {
    const char *name;
    uint8_t     id;
    const char *hex;            /* 报文(十六进制文本),源自协议汇总文档的例子 */
    double      want_value;     /* 期望重量 */
    int         want_dec;       /* 期望小数位;-1 = 不校验 */
    int         want_neg;       /* 期望负号;-1 = 不校验 */
    int         want_stable;    /* 期望稳定;-1 = 不校验 */
    int         want_ck;        /* 期望校验;-1 = 不校验 */
    double      want_tare;      /* >0:顺便校验皮重 */
    uint8_t     override_dec;   /* >0:覆盖 default_decimals(协议 6/15/16 需用户预设) */
} tc_t;

static const tc_t k_cases[] = {
    /* ---- 协议 1:6 字节二进制 BCD ---- */
    { "P1  2.365 稳定",     1, "FF 44 65 23 00 00",        2.365, 3, 0, 1, -1, 0, 0 },
    { "P1 -12.365 稳定",    1, "FF 64 65 23 01 00",      -12.365, 3, 1, 1, -1, 0, 0 },
    /* ---- 协议 2 / 8:直通视窗 ---- */
    { "P2 -2.365",          2, "2D 20 20 32 2E 33 36 35 0D",     -2.365, 3, 1, -1, -1, 0, 0 },
    { "P8 -2.365",          8, "2D 20 20 32 2E 33 36 35 0D 0A", -2.365, 3, 1, -1, -1, 0, 0 },
    /* ---- 协议 3 / 19:ASCII ST/US/OL + NT/GS ---- */
    { "P3  2.365 稳定毛重", 3, "53 54 2C 47 53 2C 20 20 20 32 2E 33 36 35 6B 67 0D 0A",   2.365, 3, 0, 1, -1, 0, 0 },
    { "P3 -12.00 不稳净重", 3, "55 53 2C 4E 53 2C 2D 20 20 31 32 2E 30 30 6B 67 0D 0A", -12.00, 2, 1, 0, -1, 0, 0 },
    { "P19 2.365 稳定毛重",19, "53 54 2C 47 53 2C 20 20 20 32 2E 33 36 35 6B 67 20 0D 0A", 2.365, 3, 0, 1, -1, 0, 0 },
    /* ---- 协议 4 / 12: '=' + 先低后高 ---- */
    { "P4  2.365",          4, "3D 35 36 33 2E 32 20 20 20",     2.365, 3, 0, -1, -1, 0, 0 },
    { "P4 -12.365",         4, "3D 35 36 33 2E 32 31 20 2D",   -12.365, 3, 1, -1, -1, 0, 0 },
    { "P12 2.365",         12, "3D 35 36 33 2E 32 30 30",        2.365, 3, 0, -1, -1, 0, 0 },
    { "P12 -12.365",       12, "3D 35 36 33 2E 32 31 2D",      -12.365, 3, 1, -1, -1, 0, 0 },
    /* ---- 协议 5 / 20:STX + 校验 ---- */
    { "P5  2.365 XOR",      5, "02 2B 30 30 32 33 36 35 33 31 41 03",    2.365, 3, 0, -1, 1, 0, 0 },
    { "P20 12.36 校验",    20, "02 2B 30 30 31 32 33 36 32 31 44 03 00", 12.36, 2, 0, -1, 1, 0, 0 },
    /* ---- 协议 6:CRLF 前缀 + 无小数点(小数位需用户预设) ---- */
    { "P6  2.365 dp=3",     6, "0A 0D 20 20 20 32 33 36 35",   2.365, 3, 0, -1, -1, 0, 3 },
    { "P6 -12.65 dp=2",     6, "0A 0D 2D 20 20 31 32 36 35",  -12.65, 2, 1, -1, -1, 0, 2 },
    /* ---- 协议 10 / 11:符号+重量+单位+CRLF ---- */
    { "P10 2.365 kg",      10, "2B 20 20 32 2E 33 36 35 20 6B 67 0D 0A",   2.365, 3, 0, -1, -1, 0, 0 },
    { "P11 2.365 kg",      11, "77 6E 30 30 30 32 2E 33 36 35 6B 67 0D 0A", 2.365, 3, 0, -1, -1, 0, 0 },
    /* ---- 协议 13:净重/皮重/毛重 ---- */
    { "P13 -2.300 皮2.000", 13,
      "02 2D 20 32 2E 33 30 30 20 20 32 2E 30 30 30 00 20 20 30 2E 37 30 30 03",
      -2.300, 3, 1, -1, -1, 2.000, 0 },
    /* ---- 协议 14:'=' + 先高后低 ---- */
    { "P14 2.365",         14, "3D 30 30 32 2E 33 36 35",    2.365, 3, 0, -1, -1, 0, 0 },
    { "P14 -12.365",       14, "3D 2D 31 32 2E 33 36 35",  -12.365, 3, 1, -1, -1, 0, 0 },
    /* ---- 协议 15 / 16:定长二进制 BCD ---- */
    { "P15 -2.008 dp=3",   15, "00 00 00 75 16 10 75 00 20 08 75 16", 2.008,  3, -1, -1, -1, 0, 3 },
    { "P15 20.060 dp=3",   15, "00 00 00 75 16 10 75 02 00 60 75 16", 20.060, 3, -1, -1, -1, 0, 3 },
    { "P16 -1.930 校验",   16, "68 00 00 00 68 10 03 00 19 30 2C 16", 1.930,  3, -1, -1, 1, 0, 3 },
    { "P16  0.000 校验",   16, "68 00 00 00 68 10 03 00 00 00 E3 16", 0.000,  3, -1, -1, 1, 0, 3 },
    { "P16 20.070 校验",   16, "68 00 00 00 68 10 03 02 00 70 55 16", 20.070, 3, -1, -1, 1, 0, 3 },
    /* ---- 协议 17:STX + 小数点位 + 状态 ---- */
    { "P17 2.365",         17, "02 25 30 20 30 30 32 33 36 35 0D 0A",  2.365, 3, 0, -1, -1, 0, 0 },
    { "P17 -2.365",        17, "02 25 32 20 30 30 32 33 36 35 0D 0A", -2.365, 3, 1, -1, -1, 0, 0 },
    /* ---- 协议 18:MT-B520(文档例子的小数位与它自己的规则冲突,见 问题与版本优化.md) ---- */
    { "P18 净2365 皮1800", 18, "02 3B 32 20 30 30 32 33 36 35 30 30 31 38 30 30 0D F5",
      -236.5, 1, 1, -1, 1, 180.0, 0 },
};

int main(void)
{
    size_t i;
    size_t n = sizeof(k_cases) / sizeof(k_cases[0]);

    printf("data_proto 单测:共 %u 例\n\n", (unsigned)n);
    for (i = 0; i < n; i++) {
        const tc_t *tc = &k_cases[i];
        const data_proto_t *p = data_proto_by_id(tc->id);
        data_proto_t local;
        uint8_t frame[64];
        size_t len;
        scale_value_t v;

        if (p == NULL) {
            printf("FAIL %-22s 找不到协议 %u\n", tc->name, tc->id);
            g_fails++;
            continue;
        }
        local = *p;
        if (tc->override_dec > 0) {
            local.default_decimals = tc->override_dec;
        }
        len = hex2bin(tc->hex, frame, sizeof(frame));
        scale_value_reset(&v);
        g_checks++;

        if (!local.parse(&local, frame, len, &v) || !v.valid) {
            printf("FAIL %-22s 解析失败(len=%u)\n", tc->name, (unsigned)len);
            g_fails++;
        } else if (fabs(v.value - tc->want_value) > 1e-6) {
            printf("FAIL %-22s value=%.4f 期望 %.4f\n", tc->name, v.value, tc->want_value);
            g_fails++;
        } else if (tc->want_dec >= 0 && v.decimals != (uint8_t)tc->want_dec) {
            printf("FAIL %-22s decimals=%u 期望 %d\n", tc->name, v.decimals, tc->want_dec);
            g_fails++;
        } else if (tc->want_neg >= 0 && v.negative != (tc->want_neg != 0)) {
            printf("FAIL %-22s negative=%d 期望 %d\n", tc->name, (int)v.negative, tc->want_neg);
            g_fails++;
        } else if (tc->want_stable >= 0 && v.stable != (tc->want_stable != 0)) {
            printf("FAIL %-22s stable=%d 期望 %d\n", tc->name, (int)v.stable, tc->want_stable);
            g_fails++;
        } else if (tc->want_ck >= 0 && v.checksum_ok != (tc->want_ck != 0)) {
            printf("FAIL %-22s checksum=%d 期望 %d\n", tc->name, (int)v.checksum_ok, tc->want_ck);
            g_fails++;
        } else if (tc->want_tare > 0 && fabs(v.tare - tc->want_tare) > 1e-6) {
            printf("FAIL %-22s tare=%.4f 期望 %.4f\n", tc->name, v.tare, tc->want_tare);
            g_fails++;
        } else {
            printf("ok   %-22s value=%-10.4f dec=%u %s%s%s%s unit='%s'\n", tc->name, v.value,
                   v.decimals, v.negative ? "负 " : "", v.stable ? "稳 " : "",
                   v.is_gross ? "毛重 " : "", v.checksum_ok ? "" : "校验错 ", v.unit);
        }
    }

    printf("\n结果: 共 %d 例, 失败 %d\n", g_checks, g_fails);
    return (g_fails == 0) ? 0 : 1;
}

