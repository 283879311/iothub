#pragma once

#include <stdint.h>

/* host 单测替身:时间可注入。
 * 过期判定(now_ms >= expire_ms)的边界必须先能精确拨到 expire_ms,
 * 否则 "> 还是 >=" 这个差别在测试里永远暴露不出来。 */
int64_t esp_timer_get_time(void);

/* 测试专用:设置/推进假时钟(毫秒) */
void test_clock_set_ms(int64_t ms);
void test_clock_advance_ms(int64_t delta);
