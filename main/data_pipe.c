#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

#include "data_pipe.h"
#include "http_priv.h"
#include "mem_guard.h"

static const char *TAG = "data_pipe";

#define DATA_PIPE_QUEUE_DEPTH 16  /* 1.3-07:8→16(+≈2KB BSS),满则丢帧并计数 */
#define DATA_PIPE_TASK_STACK  4096
#define DATA_PIPE_TASK_PRIO   4

typedef struct {
    uint8_t  port;
    uint16_t len;
    uint8_t  data[DATA_PIPE_CHUNK_MAX];
} data_pipe_msg_t;

typedef struct {
    data_port_write_fn write;
    void *ctx;
    uint32_t route_mask;       /* 当前生效路由 */
    uint32_t configured_mask;  /* NVS 落位的持久路由(回环恢复用) */
} data_pipe_port_t;

static data_pipe_port_t s_ports[DATA_PORT_MAX];
static StaticQueue_t s_queue_buf;
static uint8_t s_queue_storage[DATA_PIPE_QUEUE_DEPTH * sizeof(data_pipe_msg_t)];
static QueueHandle_t s_queue = NULL;
static bool s_enabled = false;
static bool s_loopback = false;   /* 1.3-03 验收用回环开关(运行期,不落 NVS) */
static data_pipe_stats_t s_stats;

/* ---------------------------------------------------------------------------
 * 唯一出站执行者:按路由位图把字节写到对端端口
 * (运行在本任务上下文 → GATT 回调/UART 读者/MQTT 事件都不被阻塞)
 * ------------------------------------------------------------------------- */
static void data_pipe_task(void *arg)
{
    data_pipe_msg_t msg;

    (void)arg;
    for (;;) {
        unsigned dst;

        if (xQueueReceive(s_queue, &msg, portMAX_DELAY) != pdTRUE) {
            continue;
        }
        if (msg.len == 0 || msg.port >= DATA_PORT_MAX) {
            continue;
        }
        /* 1.3-44 时间片让渡:出站是数据面的常态负载,但页面流式发送与内存
         * 深紧张是"瞬时独占资源"的窗口 —— 这两个时期把出站节奏按帧放慢,
         * 让 lwIP/协议栈先把手上的活干完;只延时,不丢弃(丢帧语义仍由
         * dropped/overrun 承担,计数 yielded 供 diag 观测)。
         * 256B/帧 × 5ms ≈ 51KB/s,高于 UART1 115200 波特率(≈11.5KB/s),
         * 正常数据速率下让渡不构成瓶颈。 */
        if (app_http_serving_page()) {
            vTaskDelay(pdMS_TO_TICKS(5));
            s_stats.yielded++;
        } else if (app_mem_level() == APP_MEM_CRITICAL) {
            vTaskDelay(pdMS_TO_TICKS(10));
            s_stats.yielded++;
        }
        for (dst = 0; dst < DATA_PORT_MAX; dst++) {
            data_pipe_port_t *p;

            if ((s_ports[msg.port].route_mask & DATA_PORT_MASK(dst)) == 0) {
                continue;
            }
            p = &s_ports[dst];
            if (p->write == NULL) {
                continue;
            }
            if (p->write(msg.data, msg.len, p->ctx)) {
                s_stats.tx[dst] += msg.len;
            } else {
                s_stats.dropped[dst] += msg.len;
            }
        }
    }
}

esp_err_t data_pipe_init(void)
{
    if (s_queue != NULL) {
        return ESP_OK;
    }
    s_queue = xQueueCreateStatic(DATA_PIPE_QUEUE_DEPTH, sizeof(data_pipe_msg_t),
                                 s_queue_storage, &s_queue_buf);
    if (s_queue == NULL) {
        return ESP_ERR_NO_MEM;
    }
    s_stats.queue_depth = DATA_PIPE_QUEUE_DEPTH;
    if (xTaskCreate(data_pipe_task, "data_pipe", DATA_PIPE_TASK_STACK, NULL,
                    DATA_PIPE_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "data pipe ready (chunk=%u, depth=%u)",
             (unsigned)DATA_PIPE_CHUNK_MAX, (unsigned)DATA_PIPE_QUEUE_DEPTH);
    return ESP_OK;
}

esp_err_t data_pipe_register_port(data_port_t port, data_port_write_fn write, void *ctx)
{
    if (port >= DATA_PORT_MAX || write == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    s_ports[port].write = write;
    s_ports[port].ctx = ctx;
    ESP_LOGI(TAG, "port %u registered", (unsigned)port);
    return ESP_OK;
}

bool data_pipe_port_registered(data_port_t port)
{
    return port < DATA_PORT_MAX && s_ports[port].write != NULL;
}

void data_pipe_set_route(data_port_t from, uint32_t dest_mask)
{
    if (from >= DATA_PORT_MAX) {
        return;
    }
    s_ports[from].route_mask = dest_mask & DATA_PORT_ALL_MASK;
}

void data_pipe_apply_routes(uint32_t uart1_mask, uint32_t ble_mask, uint32_t mqtt_mask)
{
    s_ports[DATA_PORT_UART1].configured_mask = uart1_mask & DATA_PORT_ALL_MASK;
    s_ports[DATA_PORT_BLE].configured_mask = ble_mask & DATA_PORT_ALL_MASK;
    s_ports[DATA_PORT_MQTT].configured_mask = mqtt_mask & DATA_PORT_ALL_MASK;
    if (!s_loopback) {
        data_pipe_set_route(DATA_PORT_UART1, s_ports[DATA_PORT_UART1].configured_mask);
        data_pipe_set_route(DATA_PORT_BLE, s_ports[DATA_PORT_BLE].configured_mask);
        data_pipe_set_route(DATA_PORT_MQTT, s_ports[DATA_PORT_MQTT].configured_mask);
    }
    ESP_LOGI(TAG, "routes applied: uart1=0x%lx ble=0x%lx mqtt=0x%lx (loopback=%d)",
             (unsigned long)s_ports[DATA_PORT_UART1].configured_mask,
             (unsigned long)s_ports[DATA_PORT_BLE].configured_mask,
             (unsigned long)s_ports[DATA_PORT_MQTT].configured_mask,
             (int)s_loopback);
}

/* 回环开关:开=每端口回写自身(含 MQTT:下行→再发布上行,自测整条 MQTT 边);
 * 关=恢复 data_pipe_apply_routes 落位的持久路由。 */
void data_pipe_set_loopback(bool on)
{
    s_loopback = on;
    if (on) {
        data_pipe_set_route(DATA_PORT_UART1, DATA_PORT_MASK(DATA_PORT_UART1));
        data_pipe_set_route(DATA_PORT_BLE, DATA_PORT_MASK(DATA_PORT_BLE));
        data_pipe_set_route(DATA_PORT_MQTT, DATA_PORT_MASK(DATA_PORT_MQTT));
    } else {
        data_pipe_set_route(DATA_PORT_UART1, s_ports[DATA_PORT_UART1].configured_mask);
        data_pipe_set_route(DATA_PORT_BLE, s_ports[DATA_PORT_BLE].configured_mask);
        data_pipe_set_route(DATA_PORT_MQTT, s_ports[DATA_PORT_MQTT].configured_mask);
    }
    ESP_LOGW(TAG, "loopback %s (uart1->uart1, ble->ble, mqtt->mqtt)", on ? "ON" : "OFF");
}

bool data_pipe_loopback(void)
{
    return s_loopback;
}

bool data_pipe_feed(data_port_t port, const uint8_t *data, size_t len)
{
    size_t offset = 0;

    if (!s_enabled || s_queue == NULL || data == NULL || len == 0 || port >= DATA_PORT_MAX) {
        return false;
    }
    while (offset < len) {
        data_pipe_msg_t msg;
        size_t n = len - offset;

        if (n > DATA_PIPE_CHUNK_MAX) {
            n = DATA_PIPE_CHUNK_MAX;
        }
        msg.port = (uint8_t)port;
        msg.len = (uint16_t)n;
        memcpy(msg.data, data + offset, n);
        if (xQueueSend(s_queue, &msg, 0) != pdTRUE) {
            s_stats.overrun[port] += (uint32_t)(len - offset);
            ESP_LOGW(TAG, "queue full (port %u), dropped %u byte(s)",
                     (unsigned)port, (unsigned)(len - offset));
            return false;
        }
        offset += n;
        {
            UBaseType_t waiting = uxQueueMessagesWaiting(s_queue);

            if (waiting > s_stats.queue_high_water) {
                s_stats.queue_high_water = (uint16_t)waiting;
            }
        }
    }
    s_stats.rx[port] += (uint32_t)len;
    return true;
}

void data_pipe_set_enabled(bool on)
{
    s_enabled = on;
    ESP_LOGI(TAG, "data pipe %s", on ? "enabled" : "disabled");
}

bool data_pipe_enabled(void)
{
    return s_enabled;
}

void data_pipe_get_stats(data_pipe_stats_t *out)
{
    if (out != NULL) {
        *out = s_stats;
    }
}
