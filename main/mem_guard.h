#pragma once

/* ============================================================================
 * 内存水位闸门（1.3-44）："全开稳态"的让路信号
 *
 * 背景：AP+STA+MQTT+BLE+UART 全开时，稳态堆余量只有 ~5-17KB、最大连续块
 * ~3-7KB；极端窗口（页面流式发送 + MQTT 断连重试 + BLE/UART 数据流叠加）
 * 会把最大连续块砸到 2KB 以下 —— 深层的分配随机失败，表现成"页面半截/
 * 面板缺块/断连风暴"，而不是一次可见的崩溃。
 *
 * 本模块把"现在有多紧"统一成三档，让**可再生负载主动让路**（时间片轮转
 * 语义），而不是让谁先分配谁赢：
 *   - APP_MEM_PRESSED ：页面流优先（HTTP 自降档由 http_core 既有闸门承担）
 *   - APP_MEM_CRITICAL：MQTT 周期遥测本拍跳过、重连尝试推迟、
 *                       data_pipe 出站按帧减速（见各调用点注释）
 * 内存充裕时全部无感（不引入任何延迟）。
 *
 * 阈值依据（实测台账，见 问题与版本优化.md）：
 *   - 1.3-40 全开稳态 free≈16.7KB；1.3-36/1.3-42 记录的"MQTT 连接态
 *     free≈5KB / largest≈2.9KB"属于**正常工作区**，绝不能触发让路 ——
 *     PRESSED 门槛定在 4KB/2.5KB 之下；
 *   - 1.3-42：largest<2048 时页面必须 503（512B 块 + pbuf 已无余量），
 *     即 CRITICAL:largest<2048 或 free<3072（给 BT/WiFi 内部动作留出牌空间）。
 * ========================================================================== */

typedef enum {
    APP_MEM_NORMAL = 0,
    APP_MEM_PRESSED,
    APP_MEM_CRITICAL,
} app_mem_level_t;

/* 当前水位（读 free + largest 两个口径，取更紧的一档） */
app_mem_level_t app_mem_level(void);
const char *app_mem_level_str(app_mem_level_t level);
