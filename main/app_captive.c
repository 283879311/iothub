#include <ctype.h>
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

/* 强制门户 DNS 劫持白名单:只对这些域名(含子域)应答 AP IP,其余回 NXDOMAIN。
 * 目的:AP 仅用于配网/调试、客户端本无外网;若对所有域名都回 AP IP,现场业务
 * 设备(如 PDA/仪表)的云上报会被吸到本机并反复重试,造成连接风暴与 socket
 * 耗尽(问题与版本优化.md 第 30 项)。新增机型 = 在此加一行。 */
static const char *const s_probe_domains[] = {
    "captive.apple.com",                        /* iOS / macOS */
    "www.appleiphonecell.com",                  /* 老版本 iOS */
    "apple.com",
    "connectivitycheck.gstatic.com",            /* Android AOSP */
    "connectivitycheck.android.com",
    "clients3.google.com",
    "connect.rom.miui.com",                     /* MIUI */
    "connectivitycheck.hicloud.com",            /* 华为 */
    "connectivitycheck.platform.hicloud.com",
    "conn1.oppomobile.com",                     /* OPPO / realme */
    "conn2.oppomobile.com",
    "wifi.vivo.com.cn",                         /* vivo */
    "wifi2.vivo.com.cn",
    "www.msftconnecttest.com",                  /* Windows 10/11 */
    "www.msftncsi.com",                         /* 老版本 Windows */
    "dns.msftncsi.com",
    "detectportal.firefox.com",                 /* Firefox */
    "use-application-dns.net",                  /* Firefox DoH canary:回 NXDOMAIN 即禁用 DoH */
    "nmcheck.gnome.org",                        /* GNOME / Ubuntu */
    "connectivity-check.ubuntu.com",
};

/* 大小写不敏感的后缀匹配:qname 等于 suffix,或以 ".suffix" 结尾(带标签边界,
 * 避免 evil-apple.com 之类误命中) */
static bool captive_dns_is_probe_domain(const char *qname)
{
    size_t i;

    if (qname == NULL || qname[0] == '\0') {
        return false;
    }
    for (i = 0; i < sizeof(s_probe_domains) / sizeof(s_probe_domains[0]); i++) {
        const char *suffix = s_probe_domains[i];
        size_t slen = strlen(suffix);
        size_t qlen = strlen(qname);
        size_t j;
        bool equal = true;

        if (qlen < slen) {
            continue;
        }
        for (j = 0; j < slen; j++) {
            if (tolower((unsigned char)qname[qlen - slen + j]) !=
                tolower((unsigned char)suffix[j])) {
                equal = false;
                break;
            }
        }
        if (!equal) {
            continue;
        }
        if (qlen == slen || qname[qlen - slen - 1] == '.') {
            return true;
        }
    }
    return false;
}

/* 最小 DNS 应答:回显问题段。
 * hijack=true(命中探测白名单):A/ANY 查询附 A 记录指向 AP 网关 IP,其余类型回空应答,
 *   让客户端回落 A 查询;
 * hijack=false(其它域名):回 NXDOMAIN(RCODE=3)并清零 ANCOUNT,让客户端快速失败,
 *   不再把业务流量灌向本机(问题与版本优化.md 第 30 项)。
 * cap 为接收缓冲区容量:应答允许比查询长,追加判断必须用 cap 而非 len
 * (问题与版本优化.md 第 26 项:误用 len 导致几乎恒发空应答,客户端拿不到 IP 从不探测)。
 * out_answers 返回实际应答记录数,供日志区分 answered/empty/nxdomain。 */
static int captive_dns_build_reply(uint8_t *buf, int len, int cap, bool hijack, int *out_answers)
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
    uint8_t answers = (hijack && (qtype == 1 || qtype == 255)) ? 1 : 0;

    buf[2] = 0x85;                                  /* QR=1 AA=1 */
    buf[3] = hijack ? 0x80 : 0x83;                  /* RA=1;白名单 RCODE=0,否则 3(NXDOMAIN) */
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

    /* 1.3-04 实测:运行期改网络会走 esp_wifi_stop/start(间隔仅 ~80ms),旧 socket 的 UDP 端口
     * 可能尚未释放。旧实现一次 bind 失败就永久退出,此后手机再也不自动弹门户
     * (串口只留一行 E captive dns bind 192.168.8.1:53 failed)。
     * 处理:SO_REUSEADDR + 有界重试;仍失败才放弃(保持 s_dns_running=false)。 */
    int sock = -1;
    int attempt;

    for (attempt = 0; attempt < 5 && s_dns_running; attempt++) {
        int one = 1;

        sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock < 0) {
            sock = -1;
        } else {
            setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
            if (bind(sock, (struct sockaddr *)&listen_addr, sizeof(listen_addr)) == 0) {
                break;
            }
            close(sock);
            sock = -1;
        }
        if (attempt == 0) {
            ESP_LOGW(TAG, "captive dns bind %s:53 retry (port busy after wifi restart?)", APP_AP_GATEWAY_IP);
        }
        vTaskDelay(pdMS_TO_TICKS(200));
    }
    if (sock < 0) {
        ESP_LOGE(TAG, "captive dns bind %s:53 failed", APP_AP_GATEWAY_IP);
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
        bool hijack = (qname_end >= 0) && captive_dns_is_probe_domain(qname);
        int reply_len = captive_dns_build_reply(buf, len, (int)sizeof(buf), hijack, &answers);
        ESP_LOGI(TAG, "dns query %s type=%u from %s -> %s",
                 qname_end >= 0 ? qname : "?", qtype,
                 inet_ntoa(((struct sockaddr_in *)&client_addr)->sin_addr),
                 reply_len <= 0 ? "ignored"
                                : (answers > 0 ? "answered" : (hijack ? "empty" : "nxdomain")));
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
        /* 1.3-44 上机实测栈余量仅 416B(栈高水位报表,原栈 3072):本任务最深路径有
         * buf[1500]+qname[128] 等栈上缓冲 —— 面向任意 AP 客户端(DNS 查询)的路径,
         * 余量过薄。3072→4096 与 uart_console 同理买 1KB 边际(成本 = 堆 +1KB)。 */
        if (xTaskCreate(captive_dns_task, "captive_dns", 4096, NULL, 4, NULL) != pdPASS) {
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
