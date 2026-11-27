#pragma once
#include <stdbool.h>
#include <stdint.h>

/* 虚拟时钟：仅测试线程/时钟驱动线程调用 */
int64_t op_test_now_us(void);
void op_test_advance_us(int64_t us);

/* 由 stub 日志回调置位：识别产品代码既有的 hook 打印 */
bool op_test_hook_entered(void);
bool op_test_hook_exited(void);
const char *op_test_hook_probe_log(void);
const char *op_test_hook_event_log(void);

/* 诊断输出开关（默认关，避免 50 次重复刷屏） */
void op_test_set_verbose(int on);

/* P1 测试：MQTT defer begin/end 调用计数。 */
void op_test_reset_defer_counts(void);
int op_test_defer_begin_count(void);
int op_test_defer_end_count(void);

/* C15 defer 诊断：控制下一次及后续 begin 返回值（默认 ESP_OK）。 */
void op_test_set_defer_begin_result(int err);
