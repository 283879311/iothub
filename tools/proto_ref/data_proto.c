#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "data_proto.h"

/* ===================== 通用工具 ===================== */

static void sv_init(scale_value_t *v)
{
    memset(v, 0, sizeof(*v));
    v->checksum_ok = true;      /* 无校验的协议默认视为通过 */
}

void scale_value_reset(scale_value_t *v)
{
    sv_init(v);
}

/* BCD 字节 → 0..99 */
static uint32_t bcd2(uint8_t b)
{
    return (uint32_t)((b >> 4) & 0x0F) * 10u + (uint32_t)(b & 0x0F);
}

static uint8_t hexval(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    return 0;
}

/* ASCII 片段 → 数值(容忍空格);**跳过前导符号**,只返回幅度,
 * 符号位由各协议单独解析(避免重复取号) */
static bool ascii_to_mag(const uint8_t *s, size_t n, double *out)
{
    char buf[32];
    size_t i = 0;
    size_t j = 0;
    bool seen_digit = false;

    while (i < n && s[i] == ' ') {
        i++;
    }
    if (i < n && (s[i] == '-' || s[i] == '+')) {
        i++;
    }
    for (; i < n && j + 1 < sizeof(buf); i++) {
        char c = (char)s[i];
        if (c == ' ') {
            continue;
        }
        if ((c >= '0' && c <= '9') || c == '.') {
            buf[j++] = c;
            if (c != '.') {
                seen_digit = true;
            }
            continue;
        }
        break;                                  /* 其它字符即停 */
    }
    if (!seen_digit) {
        return false;
    }
    buf[j] = '\0';
    *out = strtod(buf, NULL);
    return true;
}

/* 统计片段中小数点后位数 */
static uint8_t count_decimals(const uint8_t *s, size_t n)
{
    size_t i;
    for (i = 0; i < n; i++) {
        if (s[i] == '.') {
            uint8_t d = 0;
            size_t j;
            for (j = i + 1; j < n && s[j] >= '0' && s[j] <= '9'; j++) {
                d++;
            }
            return d;
        }
    }
    return 0;
}

/* ===================== 协议 1:6 字节二进制 BCD ===================== */

static bool parse_p1(const data_proto_t *self, const uint8_t *f, size_t n,
                     scale_value_t *v)
{
    uint8_t dec_code;
    uint32_t digits;
    uint8_t i;

    (void)self;
    if (n < 6 || f[0] != 0xFF) {
        return false;
    }
    dec_code = f[1] & 0x07;                     /* D0~D2:1=无小数 … 5=4 位小数 */
    if (dec_code < 1 || dec_code > 5) {
        return false;
    }
    v->decimals = (uint8_t)(dec_code - 1);
    v->negative = ((f[1] >> 5) & 1) != 0;       /* D5 */
    v->stable   = ((f[1] >> 6) & 1) != 0;       /* D6 */
    v->overload = ((f[1] >> 7) & 1) != 0;       /* D7 */
    digits = bcd2(f[4]) * 10000u + bcd2(f[3]) * 100u + bcd2(f[2]);
    v->value = (double)digits;
    for (i = 0; i < v->decimals; i++) {
        v->value /= 10.0;
    }
    if (v->negative) {
        v->value = -v->value;
    }
    v->zero = (digits == 0);
    if ((f[5] & 0x01) == 0) {                   /* D0=0 → kg/克 */
        snprintf(v->unit, sizeof(v->unit), "kg");
    }
    v->valid = true;
    return true;
}

/* ==== 协议 3 / 19:ASCII "ST,GS,  2.365kg\r\n"(19 多一个尾空格) ==== */

static bool parse_st_gs(const data_proto_t *self, const uint8_t *f, size_t n,
                        scale_value_t *v)
{
    double mag;

    (void)self;
    if (n < 18 || f[2] != ',' || f[5] != ',') {
        return false;
    }
    if (f[0] == 'S' && f[1] == 'T') {
        v->stable = true;
    } else if (f[0] == 'U' && f[1] == 'S') {
        v->stable = false;
    } else if (f[0] == 'O' && f[1] == 'L') {
        v->overload = true;
    } else {
        return false;
    }
    v->is_gross = (f[3] == 'G' && f[4] == 'S');
    v->negative = (f[6] == '-');
    if (!ascii_to_mag(&f[7], 7, &mag)) {        /* 字节 8-14:7 字节重量 */
        return false;
    }
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(&f[7], 7);
    memcpy(v->unit, &f[14], 2);                 /* 字节 15-16:单位 */
    v->unit[2] = '\0';
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 2(9B,CR) / 8(10B,CRLF):直通视窗,不带稳定标志 ==== */

static bool parse_wedge(const data_proto_t *self, const uint8_t *f, size_t n,
                        scale_value_t *v)
{
    double mag;

    if (n < 9 || f[8] != 0x0D) {                /* 两种变体 CR 都在第 9 字节 */
        return false;
    }
    if (self->fixed_len == 10 && (n < 10 || f[9] != 0x0A)) {
        return false;
    }
    v->negative = (f[0] == '-');
    if (!ascii_to_mag(&f[1], 7, &mag)) {
        return false;
    }
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(&f[1], 7);
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 6:0x0A 0x0D + 符号 + 6 位数字(无小数点,小数位用户预设) ==== */

static bool parse_p6(const data_proto_t *self, const uint8_t *f, size_t n,
                     scale_value_t *v)
{
    uint32_t digits = 0;
    uint8_t i;
    size_t k;

    if (n < 9 || f[0] != 0x0A || f[1] != 0x0D) {
        return false;
    }
    v->negative = (f[2] == '-');
    for (k = 3; k < 9; k++) {
        if (f[k] == ' ') {
            continue;
        }
        if (f[k] < '0' || f[k] > '9') {
            return false;
        }
        digits = digits * 10u + (uint32_t)(f[k] - '0');
    }
    v->decimals = self->default_decimals;
    v->value = (double)digits;
    for (i = 0; i < v->decimals; i++) {
        v->value /= 10.0;
    }
    if (v->negative) {
        v->value = -v->value;
    }
    v->zero = (digits == 0);
    v->valid = true;
    return true;
}

/* ==== 协议 5:0x02 符号 6位 小数位 校验2位 0x03(校验=字节2..9 异或) ==== */

static bool parse_p5(const data_proto_t *self, const uint8_t *f, size_t n,
                     scale_value_t *v)
{
    uint32_t digits = 0;
    uint8_t x = 0;
    uint8_t i;
    size_t k;

    (void)self;
    if (n < 12 || f[0] != 0x02 || f[11] != 0x03) {
        return false;
    }
    if (f[1] != '+' && f[1] != '-' && f[1] != ' ') {
        return false;
    }
    v->negative = (f[1] == '-');
    for (k = 2; k < 8; k++) {
        if (f[k] < '0' || f[k] > '9') {
            return false;
        }
        digits = digits * 10u + (uint32_t)(f[k] - '0');
    }
    if (f[8] < '0' || f[8] > '4') {
        return false;
    }
    v->decimals = (uint8_t)(f[8] - '0');
    for (k = 1; k <= 8; k++) {
        x ^= f[k];
    }
    v->checksum_ok = ((uint8_t)(hexval((char)f[9]) * 16 + hexval((char)f[10])) == x);
    v->value = (double)digits;
    for (i = 0; i < v->decimals; i++) {
        v->value /= 10.0;
    }
    if (v->negative) {
        v->value = -v->value;
    }
    v->zero = (digits == 0);
    v->valid = true;
    return true;
}

/* ==== 协议 9:SOH STX STA SIGN WEIGHT(6) UNIT(2) BCC ETX EOT [STA2] ==== */

static bool parse_p9(const data_proto_t *self, const uint8_t *f, size_t n,
                     scale_value_t *v)
{
    double mag;
    uint8_t x = 0;
    size_t k;

    (void)self;
    if (n < 15 || f[0] != 0x01 || f[1] != 0x02 || f[13] != 0x03 || f[14] != 0x04) {
        return false;
    }
    switch (f[2]) {
    case 'S': v->stable = true; break;
    case 'U': v->stable = false; break;
    case 'F': v->overload = true; break;        /* 溢出或开机未归零 */
    default: return false;
    }
    v->negative = (f[3] == '-');
    if (!ascii_to_mag(&f[4], 6, &mag)) {        /* WEIGHT:W4W3.W2W1W0 */
        return false;
    }
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(&f[4], 6);
    memcpy(v->unit, &f[10], 2);
    v->unit[2] = '\0';
    for (k = 2; k <= 11; k++) {                 /* BCC = STA..UNIT 异或 */
        x ^= f[k];
    }
    v->checksum_ok = (x == f[12]);
    v->zero = (mag == 0.0);
    if (n >= 16) {                              /* 主动发送多带一个 STA2 */
        v->zero      = ((f[15] >> 4) & 1) != 0;
        v->tare_mode = ((f[15] >> 5) & 1) != 0;
        v->overload  = v->overload || (((f[15] >> 6) & 1) != 0);
    }
    v->valid = true;
    return true;
}

/* ==== 协议 7:应答式 —— 上位机发 'P';负值/不稳回 5 个 '?' ==== */

static const uint8_t p7_poll[] = { 'P' };

static bool parse_p7(const data_proto_t *self, const uint8_t *f, size_t n,
                     scale_value_t *v)
{
    double mag;

    (void)self;
    if (n >= 5 && f[0] == '?' && f[1] == '?' && f[2] == '?' && f[3] == '?' && f[4] == '?') {
        v->stable = false;                      /* 协议约定:负值或不稳 */
        v->valid = true;
        return true;
    }
    if (n < 11 || f[0] != 0x02) {
        return false;
    }
    if (!ascii_to_mag(&f[1], 7, &mag)) {
        return false;
    }
    v->negative = false;                        /* 负值/不稳走 5×'?' 分支,此分支恒为非负 */
    v->value = mag;
    v->decimals = count_decimals(&f[1], 7);
    memcpy(v->unit, &f[9], 2);
    v->unit[2] = '\0';
    v->stable = true;
    v->zero = (v->value == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 10:符号 + 7 字节重量 + 空格 + 单位 + CRLF ==== */

static bool parse_p10(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    double mag;

    (void)self;
    if (n < 13 || f[8] != ' ' || f[11] != 0x0D || f[12] != 0x0A) {
        return false;
    }
    if (f[0] != '+' && f[0] != '-' && f[0] != ' ') {
        return false;
    }
    v->negative = (f[0] == '-');
    if (!ascii_to_mag(&f[1], 7, &mag)) {
        return false;
    }
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(&f[1], 7);
    memcpy(v->unit, &f[9], 2);
    v->unit[2] = '\0';
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 11:"wn" + 8 字节重量 + 单位 + CRLF ==== */

static bool parse_p11(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    double mag;

    (void)self;
    if (n < 14 || f[0] != 'w' || f[1] != 'n' || f[12] != 0x0D || f[13] != 0x0A) {
        return false;
    }
    v->negative = (f[2] == '-');
    if (!ascii_to_mag(&f[2], 8, &mag)) {
        return false;
    }
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(&f[2], 8);
    memcpy(v->unit, &f[10], 2);
    v->unit[2] = '\0';
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 12:"=" + 先低后高 6 字节 + 符号位(第 8 字节) ==== */

static bool parse_p12(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    uint8_t rev[8];
    double mag;
    size_t i;

    (void)self;
    if (n < 8 || f[0] != '=') {
        return false;
    }
    for (i = 0; i < 6; i++) {                   /* 逆序还原 */
        rev[i] = f[1 + (5 - i)];
    }
    rev[6] = '\0';
    if (!ascii_to_mag(rev, 6, &mag)) {
        return false;
    }
    v->negative = (f[7] == '-');
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(rev, 6);
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 13:0x02 + 净重7 + 皮重7 + 0x00 + 毛重7 + 0x03 ==== */

static bool parse_p13(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    double net;
    double tare;
    double gross;

    (void)self;
    if (n < 24 || f[0] != 0x02 || f[15] != 0x00 || f[23] != 0x03) {
        return false;
    }
    if (!ascii_to_mag(&f[1], 7, &net) ||
        !ascii_to_mag(&f[8], 7, &tare) ||
        !ascii_to_mag(&f[16], 7, &gross)) {
        return false;
    }
    v->negative = (f[1] == '-');
    v->value = v->negative ? -net : net;
    v->tare = tare;
    v->gross = (f[16] == '-') ? -gross : gross;  /* 毛重符号在字节 17 */
    v->decimals = count_decimals(&f[1], 7);
    v->zero = (net == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 14:"=" + 先高后低 7 字节(第 2 字节可为 '-') ==== */

static bool parse_p14(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    double mag;

    (void)self;
    if (n < 8 || f[0] != '=') {
        return false;
    }
    v->negative = (f[1] == '-');
    if (!ascii_to_mag(&f[1], 7, &mag)) {
        return false;
    }
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(&f[1], 7);
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 4:"=" + 先低后高 6 字节 + 填充 + 符号位(第 9 字节) ==== */

static bool parse_p4(const data_proto_t *self, const uint8_t *f, size_t n,
                     scale_value_t *v)
{
    uint8_t rev[8];
    double mag;
    size_t i;

    (void)self;
    if (n < 9 || f[0] != '=') {
        return false;
    }
    for (i = 0; i < 6; i++) {
        rev[i] = f[1 + (5 - i)];
    }
    rev[6] = '\0';
    if (!ascii_to_mag(rev, 6, &mag)) {
        return false;
    }
    v->negative = (f[8] == '-');
    v->value = v->negative ? -mag : mag;
    v->decimals = count_decimals(rev, 6);
    v->zero = (mag == 0.0);
    v->valid = true;
    return true;
}

/* ==== 协议 15:0x00*3 75 16 10 75 + 组合BCD(高→低) + 75 16 ==== */

static const uint8_t p15_head[7] = { 0x00, 0x00, 0x00, 0x75, 0x16, 0x10, 0x75 };

static bool parse_p15(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    uint32_t digits;
    uint8_t i;

    if (n < 12 || memcmp(f, p15_head, sizeof(p15_head)) != 0 ||
        f[10] != 0x75 || f[11] != 0x16) {
        return false;
    }
    digits = bcd2(f[7]) * 10000u + bcd2(f[8]) * 100u + bcd2(f[9]);
    v->decimals = self->default_decimals;
    v->value = (double)digits;
    for (i = 0; i < v->decimals; i++) {
        v->value /= 10.0;
    }
    v->zero = (digits == 0);
    v->valid = true;
    return true;
}

/* ==== 协议 16:0x68 00 00 00 68 10 03 + 组合BCD + 累加和 + 0x16 ==== */

static const uint8_t p16_head[7] = { 0x68, 0x00, 0x00, 0x00, 0x68, 0x10, 0x03 };

static bool parse_p16(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    uint32_t digits;
    uint8_t sum = 0;
    uint8_t i;

    if (n < 12 || memcmp(f, p16_head, sizeof(p16_head)) != 0 || f[11] != 0x16) {
        return false;
    }
    for (i = 0; i <= 9; i++) {                  /* 校验 = 字节 1..10 累加和 */
        sum = (uint8_t)(sum + f[i]);
    }
    v->checksum_ok = (sum == f[10]);
    digits = bcd2(f[7]) * 10000u + bcd2(f[8]) * 100u + bcd2(f[9]);
    v->decimals = self->default_decimals;
    v->value = (double)digits;
    for (i = 0; i < v->decimals; i++) {
        v->value /= 10.0;
    }
    v->zero = (digits == 0);
    v->valid = true;
    return true;
}

/* ==== 协议 17:0x02 小数点位 状态 0x20 重量(6 ASCII) CR LF ==== */

static bool parse_p17(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    uint32_t digits = 0;
    size_t k;

    (void)self;
    if (n < 12 || f[0] != 0x02 || f[3] != 0x20 ||
        f[10] != 0x0D || f[11] != 0x0A) {
        return false;
    }
    if (f[1] < 0x22 || f[1] > 0x25) {           /* 0x22=无小数 … 0x25=3 位 */
        return false;
    }
    v->decimals = (uint8_t)(f[1] - 0x22);
    if (((f[2] >> 3) & 0x1F) != 0x06) {         /* D7-D3 固定 00110 */
        return false;
    }
    v->negative = ((f[2] >> 1) & 1) != 0;
    v->overload = ((f[2] >> 2) & 1) != 0;
    for (k = 4; k <= 9; k++) {
        if (f[k] < '0' || f[k] > '9') {
            return false;
        }
        digits = digits * 10u + (uint32_t)(f[k] - '0');
    }
    v->value = (double)digits;
    for (k = 0; k < v->decimals; k++) {
        v->value /= 10.0;
    }
    if (v->negative) {
        v->value = -v->value;
    }
    v->zero = (digits == 0);
    v->unit[0] = 'k'; v->unit[1] = 'g'; v->unit[2] = '\0';   /* 该协议单位固定 kg */
    v->valid = true;
    return true;
}

/* ==== 协议 18:MT-B520:0x02 小数/分度 状态 0x20 净重6 皮重6 CR 校验和 ==== */

static bool parse_p18(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    uint32_t net = 0;
    uint32_t tare = 0;
    uint8_t sum = 0;
    uint8_t dp_code;
    size_t k;

    (void)self;
    if (n < 18 || f[0] != 0x02 || f[3] != 0x20 || f[16] != 0x0D) {
        return false;
    }
    dp_code = f[1] & 0x07;                      /* D2-D0:2=无小数 … 6=4 位 */
    if (dp_code < 2 || dp_code > 6) {
        return false;
    }
    v->decimals = (uint8_t)(dp_code - 2);
    if (((f[1] >> 5) & 0x07) != 0x01) {         /* D7-D5 固定 001 */
        return false;
    }
    if (((f[2] >> 3) & 0x1F) != 0x06) {         /* D7-D3 固定 00110 */
        return false;
    }
    v->negative = ((f[2] >> 1) & 1) != 0;
    v->overload = ((f[2] >> 2) & 1) != 0;
    for (k = 4; k <= 9; k++) {
        if (f[k] < '0' || f[k] > '9') {
            return false;
        }
        net = net * 10u + (uint32_t)(f[k] - '0');
    }
    for (k = 10; k <= 15; k++) {
        if (f[k] < '0' || f[k] > '9') {
            return false;
        }
        tare = tare * 10u + (uint32_t)(f[k] - '0');
    }
    for (k = 0; k <= 16; k++) {                 /* 校验 = 字节 1..17 累加和 */
        sum = (uint8_t)(sum + f[k]);
    }
    v->checksum_ok = (sum == f[17]);
    v->value = (double)net;
    v->tare = (double)tare;
    for (k = 0; k < v->decimals; k++) {
        v->value /= 10.0;
        v->tare /= 10.0;
    }
    if (v->negative) {
        v->value = -v->value;
    }
    v->gross = v->value + v->tare;
    v->zero = (net == 0);
    v->valid = true;
    return true;
}

/* ==== 协议 20:0x02 符号 6位ASCII 小数位 '1' 校验 0x03 0x00 ==== */

static bool parse_p20(const data_proto_t *self, const uint8_t *f, size_t n,
                      scale_value_t *v)
{
    uint32_t digits = 0;
    size_t k;

    (void)self;
    if (n < 13 || f[0] != 0x02 || f[9] != '1' || f[11] != 0x03 || f[12] != 0x00) {
        return false;
    }
    if (f[1] != '+' && f[1] != '-' && f[1] != ' ') {
        return false;
    }
    v->negative = (f[1] == '-');
    for (k = 2; k <= 7; k++) {
        if (f[k] < '0' || f[k] > '9') {
            return false;
        }
        digits = digits * 10u + (uint32_t)(f[k] - '0');
    }
    if (f[8] < '0' || f[8] > '4') {
        return false;
    }
    v->decimals = (uint8_t)(f[8] - '0');
    /* 文档:校验位 = (重量数字 + 9) 十六进制的最低一位字符(例 1236+9=0x4DD → 'D') */
    v->checksum_ok = (hexval((char)f[10]) == ((digits + 9u) & 0x0Fu));
    v->value = (double)digits;
    for (k = 0; k < v->decimals; k++) {
        v->value /= 10.0;
    }
    if (v->negative) {
        v->value = -v->value;
    }
    v->zero = (digits == 0);
    v->valid = true;
    return true;
}

/* ===================== 协议注册表 ===================== */

static const data_proto_t s_protos[] = {
    {  1, "p1_bcd_ff",       6, NULL, NULL,    0, 0, parse_p1    },
    {  2, "p2_wedge_cr",     9, NULL, NULL,    0, 0, parse_wedge },
    {  3, "p3_st_gs",       18, NULL, NULL,    0, 0, parse_st_gs },
    {  4, "p4_eq_low",       9, NULL, NULL,    0, 0, parse_p4    },
    {  5, "p5_stx_xor",     12, NULL, NULL,    0, 0, parse_p5    },
    {  6, "p6_crlf_nodp",    9, NULL, NULL,    0, 3, parse_p6    },
    {  7, "p7_polled",       0, NULL, p7_poll, 1, 0, parse_p7    },
    {  8, "p8_wedge_crlf",  10, NULL, NULL,    0, 0, parse_wedge },
    {  9, "p9_soh_stx",      0, NULL, NULL,    0, 0, parse_p9    },
    { 10, "p10_sign_unit",  13, NULL, NULL,    0, 0, parse_p10   },
    { 11, "p11_wn",         14, NULL, NULL,    0, 0, parse_p11   },
    { 12, "p12_eq_lowsign",  8, NULL, NULL,    0, 0, parse_p12   },
    { 13, "p13_net_tare",   24, NULL, NULL,    0, 0, parse_p13   },
    { 14, "p14_eq_high",     8, NULL, NULL,    0, 0, parse_p14   },
    { 15, "p15_75_16",      12, NULL, NULL,    0, 3, parse_p15   },
    { 16, "p16_68_sum_16",  12, NULL, NULL,    0, 3, parse_p16   },
    { 17, "p17_stx_dp",     12, NULL, NULL,    0, 0, parse_p17   },
    { 18, "p18_mtb520",     18, NULL, NULL,    0, 3, parse_p18   },
    { 19, "p19_st_gs_sp",   19, NULL, NULL,    0, 0, parse_st_gs },
    { 20, "p20_stx_sum9",   13, NULL, NULL,    0, 0, parse_p20   },
};

size_t data_proto_count(void)
{
    return sizeof(s_protos) / sizeof(s_protos[0]);
}

const data_proto_t *data_proto_by_index(size_t index)
{
    return (index < data_proto_count()) ? &s_protos[index] : NULL;
}

const data_proto_t *data_proto_by_id(uint8_t id)
{
    size_t i;

    for (i = 0; i < data_proto_count(); i++) {
        if (s_protos[i].id == id) {
            return &s_protos[i];
        }
    }
    return NULL;
}





