/* host 侧 ESP 基础函数替身。只实现 auth_token.c 真正用到的三个符号。 */
#include <string.h>

#include "esp_random.h"
#include "esp_timer.h"

/* ---- 假时钟 ---------------------------------------------------------- */
static int64_t s_now_ms = 1000; /* 从非 0 起,避免把"未初始化"误当成合法时刻 */

int64_t esp_timer_get_time(void)
{
    return s_now_ms * 1000;
}

void test_clock_set_ms(int64_t ms)
{
    s_now_ms = ms;
}

void test_clock_advance_ms(int64_t delta)
{
    s_now_ms += delta;
}

/* ---- 确定性伪随机(xorshift64*) --------------------------------------- */
static uint64_t s_rng = 0x9E3779B97F4A7C15ULL;

uint32_t esp_random(void)
{
    s_rng ^= s_rng >> 12;
    s_rng ^= s_rng << 25;
    s_rng ^= s_rng >> 27;
    return (uint32_t)((s_rng * 0x2545F4914F6CDD1DULL) >> 32);
}

void esp_fill_random(void *buf, size_t len)
{
    uint8_t *p = (uint8_t *)buf;
    size_t   i;

    for (i = 0; i < len; i++) {
        p[i] = (uint8_t)(esp_random() & 0xff);
    }
}
