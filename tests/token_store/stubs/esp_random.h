#pragma once

#include <stddef.h>
#include <stdint.h>

/* host 单测替身:确定性伪随机,种子由测试控制。
 *
 * 刻意不用 rand() —— 需要"同一 seed 复现同一 token 序列",否则
 * 并发用例失败时无法回放。 */
void     esp_fill_random(void *buf, size_t len);
uint32_t esp_random(void);
