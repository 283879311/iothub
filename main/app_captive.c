#include <string.h>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/inet.h"
#include "lwip/sockets.h"

#include "app_captive.h"
#include "constants.h"

static const char *TAG = "app_captive";

static bool s_dns_running;
static int s_dns_sock = -1;

/* 最小 DNS 应答:回显问题段,A 记录指向 AP 网关 IP。
 * 仅对 A/ANY 查询带应答记录,其余类型回空应答,让客户端回落 A 查询。
 * cap 为接收缓冲区容量:应答允许比查询长,追加判断必须用 cap 而非 len
 * (问题.md 第 26 项:误用 len 导致几乎恒发空应答,客户端拿不到 IP 从不探测)。
 * out_answers 返回实际应答记录数,供日志区分 answered/empty。 */
static int captive_dns_build_reply(uint8_t *buf, int len, int cap, int *out_answers)
{
    if (len < 17) {
        return -1;
    }
    uint16_t flags = (uint16_t)((buf[2] << 8) | buf[3]);
    if ((flags & 0xF800) != 0x0000) {
        return -1;                                  /* 非标准查询,不回 */
    }
    int qname_end = 12;
    while (qname_end < len && buf[qname_end] != 0) {
        qname_end += buf[qname_end] + 1;
    }
    if (qname_end + 5 > len) {
        return -1;                                  /* QNAME 未收全 */
    }
    uint16_t qtype = (uint16_t)((buf[qname_end + 1] << 8) | buf[qname_end + 2]);
    uint8_t answers = (qtype == 1 || qtype == 255) ? 1 : 0;

    buf[2] = 0x85;                                  /* QR=1 AA=1 */
    buf[3] = 0x80;                                  /* RA=1 RCODE=0 */
    buf[6] = 0x00;
    buf[7] = 0x00;                                  /* ANCOUNT 先置 0,追加成功后置 1 */
    buf[8] = 0x00;
    buf[9] = 0x00;
    buf[10] = 0x00;
    buf[11] = 0x00;

    int out = qname_end + 5;
    if (answers == 1 && out + 16 <= cap) {
        uint32_t ip = inet_addr(APP_AP_GATEWAY_IP);
        buf[out++] = 0xC0;
        buf[out++] = 0x0C;                          /* 名字压缩指针指向问题段 */
        buf[out++] = 0x00;
        buf[out++] = 0x01;                          /* Type A */
        buf[out++] = 0x00;
        buf[out++] = 0x01;                          /* Class IN */
        buf[out++] = 0x00;
        buf[out++] = 0x00;
        buf[out++] = 0x00;
        buf[out++] = 0x3C;                          /* TTL 60s */
        buf[out++] = 0x00;
        buf[out++] = 0x04;                          /* RDLENGTH */
        memcpy(&buf[out], &ip, 4);
        out += 4;
        buf[7] = 0x01;
    }
    if (out_answers != NULL) {
        *out_answers = buf[7];
    }
    return out;
}

/* 解码 QNAME(仅支持查询报文的无压缩标签),返回终止符偏移(qtype 所在位置-1),
 * 失败返回 -1 */
static int captive_dns_decode_name(const uint8_t *buf, int len, int offset,
                                   char *out, size_t out_size)
{
    size_t j = 0;

    while (offset < len) {
        uint8_t label_len = buf[offset];
        if (label_len == 0) {
            out[j] = '\0';
            return offset;
        }
        if ((label_len & 0xC0) != 0 || offset + 1 + label_len > len ||
            j + label_len + 2 > out_size) {
            return -1;
        }
        if (j > 0) {
            out[j++] = '.';
        }
        memcpy(out + j, &buf[offset + 1], label_len);
        j += label_len;
        offset += 1 + label_len;
    }
    return -1;
}

static void captive_dns_task(void *arg)
{
    uint8_t buf[1500];
    struct sockaddr_in listen_addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = inet_addr(APP_AP_GATEWAY_IP),
    };

    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0 || bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) < 0) {
        ESP_LOGE(TAG, "captive dns bind %s:53 failed", APP_AP_GATEWAY_IP);
        close(sock);
        s_dns_sock = -1;
        s_dns_running = false;
        vTaskDelete(NULL);
        return;
    }
    s_dns_sock = sock;
    ESP_LOGI(TAG, "captive dns listening on %s:53", APP_AP_GATEWAY_IP);

    while (s_dns_running) {
        struct sockaddr_storage client_addr;
        socklen_t addr_len = sizeof(client_addr);
        int len = recvfrom(sock, buf, sizeof(buf), 0,
                           (struct sockaddr *)&client_addr, &addr_len);
        if (len <= 0) {
            break;                                  /* AP_STOP shutdown 唤醒 */
        }
        char qname[128];
        int qname_end = captive_dns_decode_name(buf, len, 12, qname, sizeof(qname));
        uint16_t qtype = 0;
        if (qname_end >= 0 && qname_end + 5 <= len) {
            qtype = (uint16_t)((buf[qname_end + 1] << 8) | buf[qname_end + 2]);
        }
        int answers = 0;
        int reply_len = captive_dns_build_reply(buf, len, (int)sizeof(buf), &answers);
        ESP_LOGI(TAG, "dns query %s type=%u from %s -> %s",
                 qname_end >= 0 ? qname : "?", qtype,
                 inet_ntoa(((struct sockaddr_in *)&client_addr)->sin_addr),
                 reply_len <= 0 ? "ignored" : (answers > 0 ? "answered" : "empty"));
        if (reply_len > 0) {
            sendto(sock, buf, reply_len, 0,
                   (struct sockaddr *)&client_addr, addr_len);
        }
    }
    close(sock);
    s_dns_sock = -1;
    s_dns_running = false;
    ESP_LOGI(TAG, "captive dns stopped");
    vTaskDelete(NULL);
}

static void captive_event_handler(void *arg, esp_event_base_t base, int32_t id, void *data)
{
    if (id == WIFI_EVENT_AP_START) {
        if (s_dns_running) {
            return;
        }
        s_dns_running = true;
        if (xTaskCreate(captive_dns_task, "captive_dns", 3072, NULL, 4, NULL) != pdPASS) {
            s_dns_running = false;
            ESP_LOGE(TAG, "Failed to create captive dns task");
        }
    } else if (id == WIFI_EVENT_AP_STOP && s_dns_running) {
        s_dns_running = false;
        if (s_dns_sock >= 0) {
            shutdown(s_dns_sock, SHUT_RDWR);        /* 唤醒阻塞的 recvfrom */
        }
    }
}

void app_captive_init(void)
{
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_START,
                                               captive_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_AP_STOP,
                                               captive_event_handler, NULL));

    /* 补挂注册前的启动竞态:boot-7 的 esp_wifi_start 可能先于本初始化触发 AP_START */
    wifi_mode_t mode = WIFI_MODE_NULL;
    if (esp_wifi_get_mode(&mode) == ESP_OK && (mode & WIFI_MODE_AP) != 0) {
        captive_event_handler(NULL, WIFI_EVENT, WIFI_EVENT_AP_START, NULL);
    }
}
