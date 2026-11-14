#pragma once

#include <stdbool.h>
#include <stddef.h>

#include "esp_err.h"

/* 运行期诊断(只读观测,不改任何行为):
 *   启动时 + 每 30s 打印一行 —— 堆水位(free/min/largest)+ Wi-Fi 信道(AP/STA)
 *   + AP 客户端数。
 * 用途:排查"手机连上 AP 但网页转圈/半截""AP+STA 同时打不开"这类故障时,
 * 一次性区分三种机制:①AP 信道被 STA 抖动带走;②堆紧张(16KB 整页发不出);
 * ③空口/链路被掐(信道与客户端数会同时抖动)。
 * 失败只降级为"无诊断输出",不影响启动。 */
esp_err_t app_diag_start(void);

/* 任务栈水位快照（WIFI-OP 之外的独立诊断能力，2026-11-14）。
 *   min_bytes/min_task：s_stack_watch 内所有任务的**历史最低剩余栈**的最小值及任务名；
 *   low/low_len：<2KB 的任务列表（"tcpip=1956B mqtt=1636B"），由调用方提供缓冲，可传 NULL；
 * 返回 false = 表内任务一个都没找到。
 * 注意：uxTaskGetStackHighWaterMark 返回 StackType_t 字数，内部已 ×sizeof(StackType_t)
 * 还原为字节；本函数只读、无副作用，可在命令上下文调用（约 24 次名字查找）。 */
bool app_diag_stack_snapshot(size_t *min_bytes, const char **min_task,
                             char *low, size_t low_len);
