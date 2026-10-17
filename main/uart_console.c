#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "common.h"
#include "constants.h"
#include "data_pipe.h"
#include "http_utils.h"
#include "state.h"
#include "uart.h"
#include "uart_console.h"
#include "uart_internal.h"

static const char *TAG = "uart";

static SemaphoreHandle_t s_uart_console_mutex = NULL;
static TaskHandle_t s_uart_console_task = NULL;
static StaticSemaphore_t s_uart_console_mutex_buf;

static void app_uart_poll_console(TickType_t wait_ticks);

static void app_uart_console_task_fn(void *arg)
{
    (void)arg;

    for (;;) {
        app_uart_poll_console(0);
        vTaskDelay(pdMS_TO_TICKS(100));
    }
}

static void app_append_tail(char *dst, size_t dst_size, const char *suffix)
{
    size_t current_len;
    size_t suffix_len;
    size_t overflow;

    if (dst_size == 0) {
        return;
    }

    current_len = strlen(dst);
    suffix_len = strlen(suffix);

    if (suffix_len >= dst_size) {
        memcpy(dst, suffix + suffix_len - (dst_size - 1), dst_size - 1);
        dst[dst_size - 1] = '\0';
        return;
    }

    if (current_len + suffix_len < dst_size) {
        memcpy(dst + current_len, suffix, suffix_len + 1);
        return;
    }

    overflow = current_len + suffix_len - (dst_size - 1);
    memmove(dst, dst + overflow, current_len - overflow + 1);
    current_len = strlen(dst);
    memcpy(dst + current_len, suffix, suffix_len + 1);
}

static void app_uart_append_console(const uint8_t *raw, size_t raw_len)
{
    char plain_chunk[UART_SAMPLE_BUFFER_SIZE * 4 + 2];
    char hex_chunk[UART_SAMPLE_BUFFER_SIZE * 3 + 2];
    size_t plain_index = 0;
    size_t hex_index = 0;
    size_t index;

    for (index = 0; index < raw_len; index++) {
        uint8_t ch = raw[index];
        const char *escape = NULL;
        int written;

        if (ch == '\r' || ch == '\n' || ch == '\t' || (ch >= 32 && ch <= 126)) {
            if (plain_index + 1 < sizeof(plain_chunk)) {
                plain_chunk[plain_index++] = (char)ch;
            }
        } else {
            switch (ch) {
            case 0x00:
                escape = "\\0";
                break;
            case 0x02:
                escape = "\\x02";
                break;
            case 0x03:
                escape = "\\x03";
                break;
            default:
                break;
            }
            if (escape != NULL) {
                written = snprintf(plain_chunk + plain_index,
                                   sizeof(plain_chunk) - plain_index,
                                   "%s",
                                   escape);
            } else {
                written = snprintf(plain_chunk + plain_index,
                                   sizeof(plain_chunk) - plain_index,
                                   "\\x%02X",
                                   ch);
            }
            if (written > 0) {
                size_t append_len = (size_t)written;
                if (append_len >= sizeof(plain_chunk) - plain_index) {
                    plain_index = sizeof(plain_chunk) - 1;
                } else {
                    plain_index += append_len;
                }
            }
        }

        if (hex_index + 3 < sizeof(hex_chunk)) {
            hex_index += (size_t)snprintf(hex_chunk + hex_index,
                                          sizeof(hex_chunk) - hex_index,
                                          "%02X ",
                                          ch);
        }
    }

    if (plain_index > 0 && plain_chunk[plain_index - 1] != '\n' &&
        plain_index + 1 < sizeof(plain_chunk)) {
        plain_chunk[plain_index++] = '\n';
    }
    plain_chunk[plain_index] = '\0';

    if (hex_index > 0) {
        if (hex_chunk[hex_index - 1] == ' ') {
            hex_chunk[hex_index - 1] = '\n';
        } else if (hex_index + 1 < sizeof(hex_chunk)) {
            hex_chunk[hex_index++] = '\n';
            hex_chunk[hex_index] = '\0';
        }
    }

    app_append_tail(s_uart.rx_plain, sizeof(s_uart.rx_plain), plain_chunk);
    app_append_tail(s_uart.rx_hex, sizeof(s_uart.rx_hex), hex_chunk);
    s_uart.rx_sequence++;
}

static int app_uart_read_raw(uint8_t *raw, size_t raw_size, TickType_t wait_ticks)
{
    size_t available = 0;
    int read_len;

    if (!s_uart.driver_installed || raw_size == 0) {
        return -1;
    }

    uart_get_buffered_data_len(UART_PORT, &available);
    if (available == 0 && wait_ticks > 0) {
        vTaskDelay(wait_ticks);
        uart_get_buffered_data_len(UART_PORT, &available);
    }

    if (available == 0) {
        return 0;
    }

    if (available > raw_size) {
        available = raw_size;
    }

    read_len = uart_read_bytes(UART_PORT, raw, available, pdMS_TO_TICKS(50));
    ESP_LOGD(TAG, "uart_read: available=%u read=%d", (unsigned)available, read_len);
    return read_len;
}

/* ---- /api/v1/uart/console 的流式发送(见 问题与版本优化.md 第 45 项) ----
 *
 * 旧实现:一次请求要同时存活 3 块堆内存 —— 2049(plain_json) + 6145(hex_json)
 *   + 8448(response) = 16642 B,叠加 TCP 发送缓冲(5760 B)后超过稳态可用堆(~19.6 KB),
 *   必然返回 HTTP 500 out_of_memory。
 * 新实现:锁内做静态快照(4 KB BSS)+ 锁外 512 B 栈缓冲分片转义发送 => 堆零分配。
 * 串行化:s_uart_console_mutex 保护快照写入;httpd 为单任务,两个请求不会交错。
 */

/* 分片输出窗口:httpd 任务栈只有 4096 B(HTTPD_DEFAULT_CONFIG),512 B 输出约 8 个 chunk
 * 即可发完 4 KB 载荷(每片不会写出半个转义原子,见 app_json_escape_stream_step) */
#define CONSOLE_STREAM_OUT_BYTES 512

/* 响应快照:+1 保证末尾 NUL(即使 rx_* 恰好填满,stream 也不会越界读到相邻字段) */
static char s_console_plain_snapshot[UART_CONSOLE_PLAIN_SIZE + 1];
static char s_console_hex_snapshot[UART_CONSOLE_HEX_SIZE + 1];

static esp_err_t app_uart_send_chunk_text(httpd_req_t *req, const char *text)
{
    return httpd_resp_send_chunk(req, text, (ssize_t)strlen(text));
}

static esp_err_t app_uart_send_escaped_stream(httpd_req_t *req, const char *src)
{
    char out[CONSOLE_STREAM_OUT_BYTES];
    size_t cursor = 0;
    bool done = false;

    while (!done) {
        size_t out_len = 0;
        esp_err_t err;

        done = app_json_escape_stream_step(src, &cursor, out, sizeof(out), &out_len);
        if (out_len == 0 && !done) {
            /* 抽不动了(窗口小于最长转义原子 6 B):防御性返回,避免死循环 */
            return ESP_FAIL;
        }
        if (out_len > 0) {
            err = httpd_resp_send_chunk(req, out, (ssize_t)out_len);
            if (err != ESP_OK) {
                return err;
            }
        }
    }
    return ESP_OK;
}

esp_err_t app_uart_send_console_json(httpd_req_t *req)
{
    char tail[80];
    esp_err_t err;
    bool uart_locked = false;
    bool console_locked = false;
    bool driver_installed;
    unsigned rx_sequence;
    int tail_len;

    if (s_uart_console_mutex == NULL) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_mutex_not_ready\"}");
    }
    if (!app_uart_lock(pdMS_TO_TICKS(300))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_port_busy\"}");
    }
    uart_locked = true;
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
        app_uart_unlock();
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_console_busy\"}");
    }
    console_locked = true;

    /* ① 锁内:补一次轮询 + 快照(只做 memcpy,不做转义/发送) */
    app_uart_poll_console(0);
    memcpy(s_console_plain_snapshot, s_uart.rx_plain, UART_CONSOLE_PLAIN_SIZE);
    s_console_plain_snapshot[UART_CONSOLE_PLAIN_SIZE] = '\0';
    memcpy(s_console_hex_snapshot, s_uart.rx_hex, UART_CONSOLE_HEX_SIZE);
    s_console_hex_snapshot[UART_CONSOLE_HEX_SIZE] = '\0';
    rx_sequence = (unsigned)s_uart.rx_sequence;
    driver_installed = s_uart.driver_installed;

    /* ② 立即解锁:旧实现持锁跨越整包发送,慢客户端会顶住 UART 后台轮询 */
    if (console_locked) {
        xSemaphoreGiveRecursive(s_uart_console_mutex);
    }
    if (uart_locked) {
        app_uart_unlock();
    }

    /* ③ 锁外:分片流式发送(堆零分配) */
    tail_len = snprintf(tail, sizeof(tail),
                        "\",\"rx_sequence\":%u,\"driver_installed\":%s}",
                        rx_sequence, driver_installed ? "true" : "false");
    if (tail_len < 0 || (size_t)tail_len >= sizeof(tail)) {
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");

    err = app_uart_send_chunk_text(req, "{\"plain\":\"");
    if (err == ESP_OK) {
        err = app_uart_send_escaped_stream(req, s_console_plain_snapshot);
    }
    if (err == ESP_OK) {
        err = app_uart_send_chunk_text(req, "\",\"hex\":\"");
    }
    if (err == ESP_OK) {
        err = app_uart_send_escaped_stream(req, s_console_hex_snapshot);
    }
    if (err == ESP_OK) {
        err = app_uart_send_chunk_text(req, tail);
    }
    if (err == ESP_OK) {
        err = httpd_resp_send_chunk(req, NULL, 0);   /* 收尾:0 长度块结束响应 */
    }
    return err;
}

void app_uart_console_init(void)
{
    /* 1.3-44 上机实测栈余量仅 368B(栈高水位报表,原栈 3072):本任务最深路径有
     * raw(256B)+plain_chunk(1026B)+hex_chunk(770B) ≈ 2KB 栈上转义缓冲,属
     * 确定性开销 —— 余量虽为正但过薄,任何后续改动都可能顶穿(canary panic)。
     * 3072→4096 买 1KB 安全边际;成本 = 任务栈由堆分配(xTaskCreate)+1KB,
     * 由 1.3-46 省出的 ~1.5KB 覆盖,净仍为正。 */
    s_uart_console_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_uart_console_mutex_buf);
    if (s_uart_console_task == NULL &&
        xTaskCreate(app_uart_console_task_fn, "uart_console", 4096, NULL, 5, &s_uart_console_task) != pdPASS) {
        s_uart_console_task = NULL;
        ESP_LOGE(TAG, "Failed to create uart console task");
    }
}

void app_uart_console_reset(void)
{
    s_uart.rx_sequence = 0;
    app_copy_string(s_uart.rx_plain, sizeof(s_uart.rx_plain), "");
    app_copy_string(s_uart.rx_hex, sizeof(s_uart.rx_hex), "");
}

static void app_uart_poll_console(TickType_t wait_ticks)
{
    uint8_t raw[UART_SAMPLE_BUFFER_SIZE];
    int read_len;

    if (!app_uart_lock(pdMS_TO_TICKS(100))) {
        return;
    }
    if (!s_uart.driver_installed) {
        app_uart_unlock();
        return;
    }
    if (s_uart_console_mutex == NULL) {
        app_uart_unlock();
        return;
    }
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(100)) != pdTRUE) {
        app_uart_unlock();
        return;
    }

    read_len = app_uart_read_raw(raw, sizeof(raw), wait_ticks);
    if (read_len > 0) {
        app_uart_append_console(raw, (size_t)read_len);
        /* 数据面:同一份字节同时投给 data_pipe(不新增第二个读者,单读者原则) */
        data_pipe_feed(DATA_PORT_UART1, raw, (size_t)read_len);
    }

    xSemaphoreGiveRecursive(s_uart_console_mutex);
    app_uart_unlock();
}

esp_err_t app_uart_handle_clear_console_request(httpd_req_t *req)
{
    bool uart_locked = false;
    bool console_locked = false;

    if (s_uart_console_mutex == NULL) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_mutex_not_ready\"}");
    }
    if (!app_uart_lock(pdMS_TO_TICKS(300))) {
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_port_busy\"}");
    }
    uart_locked = true;
    if (xSemaphoreTakeRecursive(s_uart_console_mutex, pdMS_TO_TICKS(300)) != pdTRUE) {
        app_uart_unlock();
        return app_http_send_json_text(req, "503 Service Unavailable",
                                   "{\"status\":\"error\",\"message\":\"uart_console_busy\"}");
    }
    console_locked = true;
    app_uart_console_reset();
    if (console_locked) {
        xSemaphoreGiveRecursive(s_uart_console_mutex);
    }
    if (uart_locked) {
        app_uart_unlock();
    }

    return app_http_send_json_text(req, NULL,
                               "{\"status\":\"ok\",\"message\":\"uart_console_cleared\"}");
}
