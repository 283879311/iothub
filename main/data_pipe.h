#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "esp_err.h"

/* ============================================================================
 * 数据面管道(与具体链路无关):UART1 / BLE / MQTT 三边共用同一套入站/出站/路由/统计
 *
 * 设计约束(与官方/既有规则一致):
 *   - 任意上下文可调 data_pipe_feed():只做"拷贝 + 入队",绝不阻塞
 *     (GATT 回调禁止耗时操作;UART 单读者任务也不能被拖住)
 *   - 唯一的出站执行者是 data_pipe 任务:按路由位图把字节写到对端端口
 *   - 有界静态队列:满了就丢弃并计数(数据面允许丢帧,不允许撕裂/错位)
 *   - 命令帧永不进管道(BLE 行重组器/MQTT RPC 各自拦截,见 1.3-31/1.3-32)
 * ========================================================================== */

typedef enum {
    DATA_PORT_UART1 = 0,
    DATA_PORT_BLE = 1,
    DATA_PORT_MQTT = 2,
    DATA_PORT_MAX
} data_port_t;

#define DATA_PORT_MASK(p) (1u << (unsigned)(p))
#define DATA_PORT_ALL_MASK  ((1u << DATA_PORT_MAX) - 1u)

/* 单块上限(GATT 单次写入的落地容量必须 ≥ 本值,否则大帧在 BLE 写这一层被
 * 静默拒绝 —— bt_gatts.c 用 _Static_assert 钉死这个跨模块不变量,1.3-46) */
#define DATA_PIPE_CHUNK_MAX   256

/* 端口出站函数:返回 false 表示未送出(未注册/未连接/拥塞),计入 dropped */
typedef bool (*data_port_write_fn)(const uint8_t *data, size_t len, void *ctx);

typedef struct {
    uint32_t rx[DATA_PORT_MAX];      /* 各端口入站字节 */
    uint32_t tx[DATA_PORT_MAX];      /* 各端口出站字节 */
    uint32_t dropped[DATA_PORT_MAX]; /* 各端口出站未送出(未就绪/未连接) */
    uint32_t overrun[DATA_PORT_MAX]; /* 各端口入站队列满丢弃 */
    uint32_t yielded;                /* 1.3-44:出站让渡延时次数(页面流/内存紧张时主动减速) */
    uint16_t queue_depth;            /* 队列容量(块) */
    uint16_t queue_high_water;       /* 队列最高占用(块,看要不要继续加深) */
} data_pipe_stats_t;

esp_err_t data_pipe_init(void);
esp_err_t data_pipe_register_port(data_port_t port, data_port_write_fn write, void *ctx);
void data_pipe_set_route(data_port_t from, uint32_t dest_mask);

/* NVS 路由落位(1.3-07):设置"持久路由"并立即生效(回环开启时仅存档) */
void data_pipe_apply_routes(uint32_t uart1_mask, uint32_t ble_mask, uint32_t mqtt_mask);

/* 回环(1.3版本优化.md 1.3-03 验收):开 = 每端口把入站字节回写**自身**
 * (BLE<->BLE / UART1<->UART1 / MQTT<->MQTT),关 = 恢复 data_pipe_apply_routes 落位的路由。
 * 运行期生效、不落 NVS(重启即恢复,不影响产品行为)。 */
void data_pipe_set_loopback(bool on);
bool data_pipe_loopback(void);

/* 入站:任意上下文可调用(只入队)。返回 false = 被丢弃(未启用/队列满/参数错) */
bool data_pipe_feed(data_port_t port, const uint8_t *data, size_t len);

void data_pipe_set_enabled(bool on);
bool data_pipe_enabled(void);
bool data_pipe_port_registered(data_port_t port);
void data_pipe_get_stats(data_pipe_stats_t *out);
