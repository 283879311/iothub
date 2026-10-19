#pragma once

#include "esp_err.h"

/* 运行期诊断(只读观测,不改任何行为):
 *   启动时 + 每 30s 打印一行 —— 堆水位(free/min/largest)+ Wi-Fi 信道(AP/STA)
 *   + AP 客户端数。
 * 用途:排查"手机连上 AP 但网页转圈/半截""AP+STA 同时打不开"这类故障时,
 * 一次性区分三种机制:①AP 信道被 STA 抖动带走;②堆紧张(16KB 整页发不出);
 * ③空口/链路被掐(信道与客户端数会同时抖动)。
 * 失败只降级为"无诊断输出",不影响启动。 */
esp_err_t app_diag_start(void);
