#include <ctype.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

#include "common.h"
#include "config.h"
#include "data_pipe.h"
#include "state.h"
#include "uart.h"
#include "uart_internal.h"

static const char *TAG = "uart";

extern app_config_t s_config;

uart_runtime_t s_uart = {0};
static SemaphoreHandle_t s_uart_op_mutex = NULL;
static StaticSemaphore_t s_uart_op_mutex_buf;

/* 1.3-76:UART TX ring buffer。原 tx_buffer_size=0 时 uart_write_bytes 会
 * 阻塞到数据全部发出(官方 UART 文档),data_pipe 任务被拖住 → 队列 overrun。
 * 2048 > UART_HW_FIFO_LEN,ISR 后台搬 FIFO;发送前用 free_size 做非阻塞背压。 */
#define UART_DRV_TX_BUF_SIZE 2048

/* ---- UART1 入站读取(P1-11/1.3-94):UART1 → data_pipe → 路由出站 ----
 * 设计声明三边双向(data_pipe.h),此前缺入站读取——RX 环形缓冲装满即静默丢弃。
 * 本任务补齐:有界读取(256B = DATA_PIPE_CHUNK_MAX,一次 feed 一块),字节透明、
 * 不解析业务协议;队列满/未启用等丢弃与计数语义沿用 data_pipe 既有实现。
 * 与 apply 重配置互斥:uart_driver_delete/install 并发读会触碰已释放缓冲,
 * 读取全程持 app_uart_lock(递归锁,空闲 20ms 读超时即让路,apply 最多等 20ms)。 */
#define UART1_RX_READ_LEN   256
#define UART1_RX_TASK_STACK 3072
#define UART1_RX_TASK_PRIO  3

static TaskHandle_t s_uart1_rx_task;
static uint8_t s_uart1_rx_buf[UART1_RX_READ_LEN];

static void app_uart1_rx_task(void *arg)
{
    (void)arg;
    for (;;) {
        int n;

        if (!app_uart_lock(pdMS_TO_TICKS(100))) {
            continue;
        }
        n = uart_read_bytes(UART_PORT, s_uart1_rx_buf, UART1_RX_READ_LEN,
                            pdMS_TO_TICKS(20));
        app_uart_unlock();
        if (n > 0) {
            (void)data_pipe_feed(DATA_PORT_UART1, s_uart1_rx_buf, (size_t)n);
        } else if (n < 0) {
            /* 驱动未安装/重配置窗口:让路后重试 */
            vTaskDelay(pdMS_TO_TICKS(50));
        }
    }
}

static void app_uart1_rx_task_start_once(void)
{
    if (s_uart1_rx_task != NULL) {
        return;
    }
    if (xTaskCreate(app_uart1_rx_task, "uart1_rx", UART1_RX_TASK_STACK, NULL,
                    UART1_RX_TASK_PRIO, &s_uart1_rx_task) != pdPASS) {
        s_uart1_rx_task = NULL;
        ESP_LOGW(TAG, "uart1 rx task create failed (UART1 inbound disabled)");
        return;
    }
    ESP_LOGI(TAG, "uart1 rx reader started (inbound -> data_pipe, chunk=%u)",
             (unsigned)UART1_RX_READ_LEN);
}


static void app_uart_set_apply_error(const char *error_key)
{
    app_copy_string(s_uart.last_apply_error, sizeof(s_uart.last_apply_error),
                    error_key != NULL ? error_key : "uart_apply_failed");
}

static const char *app_uart_get_apply_error(void)
{
    return s_uart.last_apply_error[0] != '\0' ? s_uart.last_apply_error : "uart_apply_failed";
}

static void app_uart_build_preview_text(char *buffer, size_t buffer_size,
                                        const uint8_t *payload, size_t payload_len)
{
    static const char *hex = "0123456789ABCDEF";
    size_t index;
    size_t offset = 0;

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    buffer[0] = '\0';
    if (payload == NULL || payload_len == 0) {
        return;
    }

    for (index = 0; index < payload_len && offset + 1 < buffer_size; index++) {
        unsigned char value = payload[index];

        if (isprint(value) && value != '\\') {
            buffer[offset++] = (char)value;
            continue;
        }
        if (offset + 4 >= buffer_size) {
            break;
        }
        buffer[offset++] = '\\';
        buffer[offset++] = 'x';
        buffer[offset++] = hex[(value >> 4) & 0x0F];
        buffer[offset++] = hex[value & 0x0F];
    }

    buffer[offset] = '\0';
}

static void app_uart_record_send(const char *encoding, const char *data,
                                 const uint8_t *payload, size_t payload_len,
                                 unsigned written)
{
    s_uart.tx_sequence += 1;
    s_uart.last_bytes_sent = written;
    s_uart.last_send_at_ms = esp_timer_get_time() / 1000;
    app_copy_string(s_uart.last_tx_encoding, sizeof(s_uart.last_tx_encoding),
                    encoding != NULL ? encoding : "plain");
    app_copy_string(s_uart.last_tx_data, sizeof(s_uart.last_tx_data),
                    data != NULL ? data : "");
    app_uart_build_preview_text(s_uart.last_tx_preview, sizeof(s_uart.last_tx_preview),
                                payload, payload_len);
    app_format_hex_bytes(payload, payload_len,
                         s_uart.last_tx_hex, sizeof(s_uart.last_tx_hex), ' ');
}

bool app_uart_lock(TickType_t wait_ticks)
{
    if (s_uart_op_mutex == NULL) {
        return true;
    }
    return xSemaphoreTakeRecursive(s_uart_op_mutex, wait_ticks) == pdTRUE;
}

void app_uart_unlock(void)
{
    if (s_uart_op_mutex != NULL) {
        xSemaphoreGiveRecursive(s_uart_op_mutex);
    }
}

static bool app_uart_data_bits_valid(uint8_t data_bits)
{
    return data_bits >= 5 && data_bits <= 8;
}

static bool app_uart_stop_bits_valid(uint8_t stop_bits)
{
    return stop_bits == 1 || stop_bits == 2;
}

static const char *app_uart_parity_to_string(uart_parity_t parity)
{
    switch (parity) {
    case UART_PARITY_EVEN:
        return "even";
    case UART_PARITY_ODD:
        return "odd";
    case UART_PARITY_DISABLE:
    default:
        return "none";
    }
}

static uart_word_length_t app_uart_data_bits_to_enum(uint8_t data_bits)
{
    switch (data_bits) {
    case 5:
        return UART_DATA_5_BITS;
    case 6:
        return UART_DATA_6_BITS;
    case 7:
        return UART_DATA_7_BITS;
    case 8:
    default:
        return UART_DATA_8_BITS;
    }
}

static uart_stop_bits_t app_uart_stop_bits_to_enum(uint8_t stop_bits)
{
    return stop_bits == 2 ? UART_STOP_BITS_2 : UART_STOP_BITS_1;
}

static bool app_uart_parse_hex_bytes(const char *text, uint8_t *out, size_t out_size, size_t *out_len)
{
    size_t count = 0;
    int high_nibble = -1;

    while (*text != '\0') {
        unsigned char ch = (unsigned char)*text++;
        int value;

        if (isspace(ch) || ch == ',' || ch == ';' || ch == '-') {
            continue;
        }
        if (ch >= '0' && ch <= '9') {
            value = ch - '0';
        } else if (ch >= 'a' && ch <= 'f') {
            value = ch - 'a' + 10;
        } else if (ch >= 'A' && ch <= 'F') {
            value = ch - 'A' + 10;
        } else {
            return false;
        }

        if (high_nibble < 0) {
            high_nibble = value;
            continue;
        }

        if (count >= out_size) {
            return false;
        }
        out[count++] = (uint8_t)((high_nibble << 4) | value);
        high_nibble = -1;
    }

    if (high_nibble >= 0) {
        return false;
    }

    if (out_len != NULL) {
        *out_len = count;
    }
    return true;
}

static bool app_uart_prepare_payload(const char *data, const char *encoding,
                                     uint8_t *tx_bytes, size_t tx_bytes_size,
                                     const uint8_t **payload, size_t *payload_len)
{
    if (data == NULL || payload == NULL || payload_len == NULL) {
        return false;
    }

    if (strcmp(encoding, "hex") == 0) {
        if (!app_uart_parse_hex_bytes(data, tx_bytes, tx_bytes_size, payload_len) || *payload_len == 0) {
            return false;
        }
        *payload = tx_bytes;
        return true;
    }

    *payload = (const uint8_t *)data;
    *payload_len = strlen(data);
    return *payload_len > 0;
}

static esp_err_t app_uart_send_payload(const char *encoding, const char *data,
                                       const uint8_t *payload, size_t payload_len,
                                       unsigned *bytes_sent)
{
    int written;

    if (!s_uart.driver_installed) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        return ESP_ERR_TIMEOUT;
    }
    /* 1.3-76:TX ring 余量不足时直接返回可重试错误,不阻塞 data_pipe 任务。
     * 官方 UART 文档:tx_buffer_size>0 时 write 只负责拷入 ring,ISR 搬 FIFO;
     * 这里先看 free_size,避免 uart_write_bytes 在 ring 满时阻塞。 */
    {
        size_t tx_free = 0;

        if (uart_get_tx_buffer_free_size(UART_PORT, &tx_free) != ESP_OK ||
            tx_free < payload_len) {
            app_uart_unlock();
            return ESP_ERR_NO_MEM;
        }
    }
    written = uart_write_bytes(UART_PORT, payload, payload_len);
    app_uart_unlock();
    if (written < 0) {
        return ESP_FAIL;
    }

    app_uart_record_send(encoding, data, payload, payload_len, (unsigned)written);
    if (bytes_sent != NULL) {
        *bytes_sent = (unsigned)written;
    }
    return ESP_OK;
}

void app_uart_init(void)
{
    /* 静态创建不会失败,锁在启动单线程期就绪 */
    s_uart_op_mutex = xSemaphoreCreateRecursiveMutexStatic(&s_uart_op_mutex_buf);
}

void app_uart_build_frame(char *buffer, size_t buffer_size,
                          uint8_t data_bits, uart_parity_t parity, uint8_t stop_bits)
{
    char parity_char = 'N';

    if (buffer == NULL || buffer_size == 0) {
        return;
    }

    switch (parity) {
    case UART_PARITY_EVEN:
        parity_char = 'E';
        break;
    case UART_PARITY_ODD:
        parity_char = 'O';
        break;
    case UART_PARITY_DISABLE:
    default:
        parity_char = 'N';
        break;
    }

    snprintf(buffer, buffer_size, "%u%c%u", (unsigned)data_bits, parity_char, (unsigned)stop_bits);
}

uart_parity_t app_uart_parity_from_string(const char *value)
{
    if (strcmp(value, "even") == 0 || strcmp(value, "8E1") == 0) {
        return UART_PARITY_EVEN;
    }
    if (strcmp(value, "odd") == 0 || strcmp(value, "8O1") == 0) {
        return UART_PARITY_ODD;
    }
    return UART_PARITY_DISABLE;
}

void app_uart_reset_runtime_state(void)
{
    app_uart_set_apply_error("");
}

static esp_err_t app_uart_apply_config_values(uint32_t baudrate, uint8_t data_bits,
                                              uart_parity_t parity, uint8_t stop_bits)
{
    esp_err_t err;
    uart_sclk_t source_clk;
    uart_config_t uart_config;
    int retry;

    source_clk = (baudrate < 4800) ? UART_SCLK_REF_TICK : UART_SCLK_DEFAULT;
    memset(&uart_config, 0, sizeof(uart_config));
    uart_config.baud_rate = (int)baudrate;
    uart_config.data_bits = app_uart_data_bits_to_enum(data_bits);
    uart_config.parity = parity;
    uart_config.stop_bits = app_uart_stop_bits_to_enum(stop_bits);
    uart_config.flow_ctrl = UART_HW_FLOWCTRL_DISABLE;
    uart_config.source_clk = source_clk;

    if (!app_uart_lock(pdMS_TO_TICKS(200))) {
        app_uart_set_apply_error("uart_busy");
        return ESP_ERR_TIMEOUT;
    }

    uart_driver_delete(UART_PORT);
    s_uart.driver_installed = false;
    vTaskDelay(pdMS_TO_TICKS(20));

    app_uart_set_apply_error("uart_apply_failed");

    for (retry = 0; retry < 2; retry++) {
        if (s_uart.driver_installed) {
            uart_driver_delete(UART_PORT);
            s_uart.driver_installed = false;
        }
        if (retry > 0) {
            vTaskDelay(pdMS_TO_TICKS(20));
        }

        err = uart_param_config(UART_PORT, &uart_config);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_param_config_failed");
            ESP_LOGE(TAG, "uart_param_config failed: %s", esp_err_to_name(err));
            continue;
        }

        gpio_reset_pin(app_uart_rx_gpio());
        gpio_reset_pin(app_uart_tx_gpio());
        gpio_pullup_en(app_uart_rx_gpio());
        gpio_pulldown_dis(app_uart_rx_gpio());

        err = uart_set_pin(UART_PORT, app_uart_tx_gpio(), app_uart_rx_gpio(),
                           UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_set_pin_failed");
            ESP_LOGE(TAG, "uart_set_pin failed: %s", esp_err_to_name(err));
            continue;
        }
        /* uart_set_pin (via gpio_matrix_input) only enables input but does not
         * set pull-up. Re-enable pull-up after pin assignment so the RX line
         * is not left floating. */
        gpio_pullup_en(app_uart_rx_gpio());
        gpio_pulldown_dis(app_uart_rx_gpio());

        /* CONFIG_UART_ISR_IN_IRAM=y 时 ISR 必须驻留 IRAM;显式带上 flag,
         * 否则驱动会打印 "flag not set ... flag updated" 警告(问题与版本优化.md 第 31 项) */
        err = uart_driver_install(UART_PORT, UART_RX_BUFFER_SIZE * 2, UART_DRV_TX_BUF_SIZE, 0, NULL,
                                  ESP_INTR_FLAG_IRAM);
        if (err != ESP_OK) {
            app_uart_set_apply_error("uart_driver_install_failed");
            ESP_LOGE(TAG, "uart_driver_install failed: %s", esp_err_to_name(err));
            continue;
        }

        s_uart.driver_installed = true;
        app_uart_set_apply_error("");
            uart_flush_input(UART_PORT);
        app_uart1_rx_task_start_once();
        app_uart_unlock();
        return ESP_OK;
    }

    app_uart_unlock();
    return err;
}

esp_err_t app_uart_apply_config(void)
{
    return app_uart_apply_config_values(s_config.uart_baudrate,
                                        s_config.uart_data_bits,
                                        (uart_parity_t)s_config.uart_parity_mode,
                                        s_config.uart_stop_bits);
}

esp_err_t app_uart_send_data(const char *encoding, const char *data, unsigned *bytes_sent)
{
    uint8_t tx_bytes[UART_TX_BUFFER_SIZE / 2];
    const uint8_t *payload = NULL;
    size_t payload_len = 0;

    if (!app_uart_prepare_payload(data, encoding != NULL ? encoding : "plain",
                                  tx_bytes, sizeof(tx_bytes), &payload, &payload_len)) {
        return ESP_ERR_INVALID_ARG;
    }

    return app_uart_send_payload(encoding != NULL ? encoding : "plain",
                                 data != NULL ? data : "",
                                 payload, payload_len, bytes_sent);
}

/* ---------------------------------------------------------------------------
 * 数据面:按长度发送原始字节(二进制安全,不经 strlen)
 * 供 data_pipe 的 UART 端口使用(见 data_pipe.h)
 * ------------------------------------------------------------------------- */
esp_err_t app_uart_send_bytes(const uint8_t *data, size_t len, unsigned *bytes_sent)
{
    char preview[UART_TX_PREVIEW_SIZE];
    size_t preview_len;

    if (data == NULL || len == 0) {
        return ESP_ERR_INVALID_ARG;
    }
    /* preview 只用于日志/调试台显示;payload 传原始字节,hex 记录完整 */
    preview_len = (len < sizeof(preview) - 1) ? len : sizeof(preview) - 1;
    memcpy(preview, data, preview_len);
    preview[preview_len] = '\0';

    return app_uart_send_payload("plain", preview, data, len, bytes_sent);
}

bool app_uart_dataplane_write(const uint8_t *data, size_t len, void *ctx)
{
    unsigned sent = 0;

    (void)ctx;
    if (app_uart_send_bytes(data, len, &sent) != ESP_OK) {
        return false;
    }
    return sent == (unsigned)len;
}

