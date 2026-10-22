#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mbedtls/base64.h"
#include "mqtt_client.h"

#include "cmd.h"
#include "common.h"
#include "config.h"
#include "config_apply.h"
#include "constants.h"
#include "data_pipe.h"
#include "mem_guard.h"
#include "mqtt.h"
#include "ota.h"
#include "state.h"
#include "uart.h"
#include "wifi.h"

static const char *TAG = "mqtt";
#define MQTT_STOP_EVENT_TELEMETRY BIT0
#define MQTT_STOP_EVENT_BOOST     BIT1
#define MQTT_STOP_EVENT_RESTART   BIT2
#define MQTT_IO_EVENT             BIT3

extern app_config_t s_config;
mqtt_runtime_t s_mqtt = {0};
static volatile bool s_mqtt_connecting = false;

bool app_mqtt_connecting(void)
{
    return s_mqtt_connecting;
}

/* 配置变更入口清致命位:换 token/host 后允许重新尝试连接(1.3-43) */
void app_mqtt_clear_fatal(void)
{
    app_mqtt_lock();
    s_mqtt.fatal_error = false;
    app_mqtt_unlock();
}

/* 出站队列深度(监控/风暴观测用,未启动时为 0) */
static esp_mqtt_client_handle_t app_mqtt_client_safe(void);
static esp_mqtt_client_handle_t app_mqtt_client_acquire(void);
static void app_mqtt_client_release(void);

int app_mqtt_outbox_size(void)
{
    esp_mqtt_client_handle_t client = app_mqtt_client_acquire();
    int size = 0;

    if (client != NULL) {
        size = esp_mqtt_client_get_outbox_size(client);
        app_mqtt_client_release();
    }
    return size;
}

/* 1.3-89:低优先级 state/attributes 的有界让路。
 * outbox 积压时只延后可合并的状态，置 IO 事件表示"还有最新值待发"；
 * RPC 响应仍走各自的直接 publish，不经过此闸门。 */
#define APP_MQTT_LOW_PRIO_OUTBOX_LIMIT 2048
static uint32_t s_low_prio_outbox_defer;

static bool app_mqtt_low_prio_defer(void)
{
    int outbox = app_mqtt_outbox_size();

    if (outbox <= APP_MQTT_LOW_PRIO_OUTBOX_LIMIT) {
        return false;
    }
    s_low_prio_outbox_defer++;
    if (s_low_prio_outbox_defer <= 3u || (s_low_prio_outbox_defer % 20u) == 0u) {
        ESP_LOGW(TAG,
                 "low-prio publish deferred #%u (outbox): outbox=%d free=%u largest=%u",
                 (unsigned)s_low_prio_outbox_defer, outbox,
                 (unsigned)esp_get_free_heap_size(),
                 (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
    }
    return true;
}

/* 1.3-44:内存深紧张(CRITICAL)时被跳过的周期遥测拍数 —— diag 观测用 */
static uint32_t s_tele_shed_count;

uint32_t app_mqtt_tele_shed_count(void)
{
    return s_tele_shed_count;
}
static StaticSemaphore_t s_mqtt_lock_buf;
static StaticEventGroup_t s_mqtt_stop_events_buf;
static portMUX_TYPE s_mqtt_init_mux = portMUX_INITIALIZER_UNLOCKED;

_Static_assert(sizeof(esp_mqtt_client_handle_t) == sizeof(((mqtt_runtime_t *)0)->client),
               "esp_mqtt_client_handle_t must be same size as opaque client pointer");
_Static_assert(sizeof(TaskHandle_t) == sizeof(((mqtt_runtime_t *)0)->telemetry_task),
               "TaskHandle_t must be same size as telemetry_task pointer");
_Static_assert(sizeof(TaskHandle_t) == sizeof(((mqtt_runtime_t *)0)->boost_task),
               "TaskHandle_t must be same size as boost_task pointer");
_Static_assert(sizeof(TaskHandle_t) == sizeof(((mqtt_runtime_t *)0)->restart_task),
               "TaskHandle_t must be same size as restart_task pointer");

static void app_mqtt_ensure_runtime_init(void)
{
    app_mutex_ensure(&s_mqtt.lock, &s_mqtt_lock_buf, &s_mqtt_init_mux, true);
    app_event_group_ensure(&s_mqtt.stop_events, &s_mqtt_stop_events_buf, &s_mqtt_init_mux);
}

void app_mqtt_lock(void)
{
    app_mqtt_ensure_runtime_init();
    xSemaphoreTakeRecursive(s_mqtt.lock, portMAX_DELAY);
}

void app_mqtt_unlock(void)
{
    if (s_mqtt.lock != NULL) {
        xSemaphoreGiveRecursive(s_mqtt.lock);
    }
}

static esp_mqtt_client_handle_t app_mqtt_client_safe(void)
{
    esp_mqtt_client_handle_t h;
    app_mqtt_lock();
    h = s_mqtt.client;
    app_mqtt_unlock();
    return h;
}

/* 1.3-53:client 句柄的"借出/归还"协议 —— esp-mqtt 的 API 内部会取 MQTT_API_LOCK,
 * 而 mqtt_task 持该锁分发事件时会反向取 app_mqtt_lock,因此使用 client 期间不能
 * 持有 app_mqtt_lock(否则 ABBA 死锁)。改用引用计数:使用前借出、用毕归还;
 * restart 任务销毁旧 client 前等待借用清零。板上 coredump 实证:data_pipe 任务在
 * esp_mqtt_client_enqueue+64 解引用已被销毁的 client(LoadProhibited excvaddr=0x80)。 */
static int s_mqtt_client_users;

static esp_mqtt_client_handle_t app_mqtt_client_acquire(void)
{
    esp_mqtt_client_handle_t h;

    app_mqtt_lock();
    h = s_mqtt.client;
    if (h != NULL) {
        s_mqtt_client_users++;
    }
    app_mqtt_unlock();
    return h;
}

static void app_mqtt_client_release(void)
{
    app_mqtt_lock();
    if (s_mqtt_client_users > 0) {
        s_mqtt_client_users--;
    }
    app_mqtt_unlock();
}

static bool app_mqtt_connected_safe(void)
{
    bool ok;
    app_mqtt_lock();
    ok = s_mqtt.client != NULL && s_mqtt.connected;
    app_mqtt_unlock();
    return ok;
}

static void app_mqtt_build_uri(void)
{
    snprintf(s_mqtt.uri, sizeof(s_mqtt.uri), "%s://%s:%u",
             s_config.mqtt_use_tls ? "mqtts" : "mqtt",
             s_config.mqtt_host,
             s_config.mqtt_port);
}

static bool app_mqtt_is_thingsboard_backend(void)
{
    return strcmp(s_config.mqtt_backend, "tb_cloud") == 0 ||
           strcmp(s_config.mqtt_backend, "tb_selfhost") == 0;
}

static const char *app_mqtt_telemetry_topic(void)
{
    if (app_mqtt_is_thingsboard_backend()) {
        return "v1/devices/me/telemetry";
    }
    return "iothub/telemetry";
}

static const char *app_mqtt_rpc_topic(void)
{
    if (app_mqtt_is_thingsboard_backend()) {
        return "v1/devices/me/rpc/request/+";
    }
    return "iothub/rpc";
}

static const char *app_mqtt_attributes_topic(void)
{
    if (app_mqtt_is_thingsboard_backend()) {
        return "v1/devices/me/attributes";
    }
    return "iothub/attributes";
}

static const char *app_mqtt_shared_attr_response_topic(void)
{
    if (app_mqtt_is_thingsboard_backend()) {
        return "v1/devices/me/attributes/response/+";
    }
    return "iothub/attributes/response/+";
}

static int app_mqtt_publish_json(const char *topic, cJSON *root, bool is_telemetry)
{
    esp_mqtt_client_handle_t client;
    char *body;
    int msg_id = -1;

    if (root == NULL || topic == NULL) {
        return -1;
    }
    body = cJSON_PrintUnformatted(root);
    if (body == NULL) {
        ESP_LOGE(TAG, "cJSON_PrintUnformatted returned NULL");
        return -1;
    }
    client = app_mqtt_client_acquire();
    if (client == NULL) {
        cJSON_free(body);
        return -1;
    }
    /* 1.3-53:publish 期间不持 app_mqtt_lock(避免与 mqtt_task 事件分发的 ABBA),
     * 引用计数保证 client 不被并发销毁;计数更新单独在短锁内做 */
    msg_id = esp_mqtt_client_publish(client, topic, body, 0, 1, 0);
    app_mqtt_client_release();
    if (msg_id >= 0) {
        app_mqtt_lock();
        s_mqtt.last_msg_id = msg_id;
        if (is_telemetry) {
            s_mqtt.publish_count++;
        }
        app_mqtt_unlock();
        ESP_LOGD(TAG, "Published %s msg_id=%d len=%u count=%u",
                 is_telemetry ? "telemetry" : "attributes",
                 msg_id, (unsigned)strlen(body),
                 (unsigned)s_mqtt.publish_count);
    } else if (is_telemetry) {
        ESP_LOGW(TAG, "Publish telemetry FAILED, msg_id=%d", msg_id);
    }
    cJSON_free(body);
    return msg_id;
}

static void app_mqtt_add_common_active_online(cJSON *root)
{
    cJSON_AddBoolToObject(root, "active", true);
    cJSON_AddBoolToObject(root, "online", true);
}

static void app_mqtt_build_uart_frame_str(char *out, size_t out_size)
{
    if (out == NULL || out_size == 0) {
        return;
    }
    char frame[8];
    app_uart_build_frame(frame, sizeof(frame),
                         s_config.uart_data_bits,
                         (uart_parity_t)s_config.uart_parity_mode,
                         s_config.uart_stop_bits);
    app_copy_string(out, out_size, frame);
}

static void app_mqtt_build_client_id(void)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_mqtt.client_id, sizeof(s_mqtt.client_id),
             "iothub-%02X%02X%02X%02X%02X%02X",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* ---------------------------------------------------------------------------
 * 1.3-07 数据面 MQTT 边:
 *   up   = iothub/{mac}/data/up    设备 → 云(纯字节,data_pipe 出站)
 *   down = iothub/{mac}/data/down  云 → 设备(入站直喂 data_pipe)
 * 主题与后端类型无关(ThingsBoard 侧同样用自定义主题,与 1.3-31 协议表一致)。
 * ------------------------------------------------------------------------- */
static char s_data_up_topic[64];
static char s_data_down_topic[64];
/* 方案 A(1.3-07,TB 原生化)的静态缓冲:base64 编码 + telemetry JSON + 下行解码
 * (data_pipe 出站/入站均为单任务上下文,无重入) */
static char s_b64_buf[384];
static char s_telemetry_buf[512];
static uint8_t s_down_decode_buf[1024];

static void app_mqtt_build_data_topics(void)
{
    uint8_t mac[6] = {0};

    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_data_up_topic, sizeof(s_data_up_topic),
             "iothub/%02X%02X%02X%02X%02X%02X/data/up",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    snprintf(s_data_down_topic, sizeof(s_data_down_topic),
             "iothub/%02X%02X%02X%02X%02X%02X/data/down",
             mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
}

/* data_pipe 出站执行者:MUST 用 enqueue(纯入 outbox 立即返回)——
 * 严禁 publish():data_pipe 是唯一出站任务,被 TCP 发送阻塞会队头阻塞 UART/BLE 出站(1.3-07)。
 * 未连接时短路,防空转 outbox。 */
/* 1.3-76:data_pipe→MQTT 聚合 256B(20B/帧×50/s → 256B/≈4/s),降低发布频率与
 * 每帧 outbox/TCP 开销;outbox 阈值改用字节 —— esp_mqtt_client_get_outbox_size()
 * 返回字节,原 `>16` 实际只有 16 字节,几乎每帧都被误判为背压(官方 esp-mqtt
 * 文档:outbox limit 是字节;数据面用 QoS1 保证已接受的消息收到 PUBACK)。 */
#define APP_MQTT_DP_AGG_BYTES        256
#define APP_MQTT_DP_OUTBOX_LIMIT     4096
#define APP_MQTT_DP_FLUSH_PERIOD_MS  250
/* 1.3-82:数据面必须用 QoS1 + store=true。
 * 官方 esp_mqtt_client_enqueue 对 QoS0 & store=false 不会入 outbox,而是把消息
 * 留在 connection.outbound_message 并返回 -1("not enqueued");原实现把它当异步发布,
 * 既误判成功又漏发。QoS1 会复制到 outbox,由 MQTT 任务发送并等 PUBACK;
 * 返回值是 message id(>0),不是 ESP_OK。 */
#define APP_MQTT_DP_QOS              1
#define APP_MQTT_DP_STORE            true

static uint8_t s_dp_agg[APP_MQTT_DP_AGG_BYTES];
static size_t s_dp_agg_len;
static portMUX_TYPE s_dp_agg_mux = portMUX_INITIALIZER_UNLOCKED;

/* 1.3-82 诊断:把 data_pipe 的 dropped_mqtt 拆成可解释原因。data_pipe 只看到
 * app_mqtt_dataplane_write 的 bool,无法区分 outbox 满、未连接、编码失败或
 * enqueue 失败;定时 flush 失败此前完全静默。计数均为字节,日志每 8 次打一条。 */
static uint32_t s_dp_drop_outbox_bytes;
static uint32_t s_dp_drop_enqueue_bytes;
static uint32_t s_dp_drop_encode_bytes;
static uint32_t s_dp_drop_notconn_bytes;
static uint32_t s_dp_flush_fail_bytes;
static uint32_t s_dp_drop_events;

static void app_mqtt_dp_note_drop(const char *reason, size_t bytes, int outbox, int rc)
{
    if (strcmp(reason, "outbox") == 0) {
        s_dp_drop_outbox_bytes += (uint32_t)bytes;
    } else if (strcmp(reason, "enqueue") == 0) {
        s_dp_drop_enqueue_bytes += (uint32_t)bytes;
    } else if (strcmp(reason, "encode") == 0) {
        s_dp_drop_encode_bytes += (uint32_t)bytes;
    } else {
        s_dp_drop_notconn_bytes += (uint32_t)bytes;
    }
    s_dp_drop_events++;
    if ((s_dp_drop_events % 8u) == 1u) {
        ESP_LOGW(TAG,
                 "dataplane drop[%s] %uB outbox=%d rc=%d | totals outbox=%u enqueue=%u encode=%u notconn=%u flush=%u",
                 reason, (unsigned)bytes, outbox, rc,
                 (unsigned)s_dp_drop_outbox_bytes,
                 (unsigned)s_dp_drop_enqueue_bytes,
                 (unsigned)s_dp_drop_encode_bytes,
                 (unsigned)s_dp_drop_notconn_bytes,
                 (unsigned)s_dp_flush_fail_bytes);
    }
}

static bool app_mqtt_dp_send(const uint8_t *data, size_t len)
{
    esp_mqtt_client_handle_t client;
    const char *topic;
    const char *body;
    size_t body_len;
    size_t olen = 0;

    if (data == NULL || len == 0 || len > 0x7FFFFFFFu) {
        return false;
    }
    /* 1.3-53:借出引用(client 销毁被挂起),直到本帧 enqueue 完毕才归还 */
    client = app_mqtt_client_acquire();
    if (client == NULL || !app_mqtt_connected_safe()) {
        if (client != NULL) {
            app_mqtt_client_release();
        }
        app_mqtt_dp_note_drop("notconn", len, -1, 0);
        return false;
    }

    /* 字节口径的 outbox 背压:遥测/属性/数据面 QoS1 都占 outbox。
     * 超过 4KB 才丢,避免原 >16 字节的误判;outbox 满时 enqueue 返回 -2。 */
    {
        int outbox = esp_mqtt_client_get_outbox_size(client);

        if (outbox > APP_MQTT_DP_OUTBOX_LIMIT) {
            app_mqtt_dp_note_drop("outbox", len, outbox, 0);
            app_mqtt_client_release();
            return false;
        }
    }

    if (app_mqtt_is_thingsboard_backend()) {
        /* 方案 A(2026-10-15):TB broker 不路由自定义主题(实测:自订自发收不到,
         * 静默丢弃)→ 上行改 v1/devices/me/telemetry,载荷 {"data":"base64"} */
        int rc = mbedtls_base64_encode((unsigned char *)s_b64_buf, sizeof(s_b64_buf),
                                       &olen, data, len);
        int n;

        if (rc != 0 || olen >= sizeof(s_b64_buf)) {
            app_mqtt_dp_note_drop("encode", len, -1, rc);
            app_mqtt_client_release();
            return false;
        }
        s_b64_buf[olen] = '\0';
        n = snprintf(s_telemetry_buf, sizeof(s_telemetry_buf), "{\"data\":\"%s\"}", s_b64_buf);
        if (n < 0 || (size_t)n >= sizeof(s_telemetry_buf)) {
            app_mqtt_dp_note_drop("encode", len, -1, n);
            app_mqtt_client_release();
            return false;
        }
        topic = "v1/devices/me/telemetry";
        body = s_telemetry_buf;
        body_len = (size_t)n;
    } else {
        /* custom 后端:原始字节走自定义主题(裸 broker 可路由) */
        topic = s_data_up_topic;
        body = (const char *)data;
        body_len = len;
    }

    {
        int rc = esp_mqtt_client_enqueue(client, topic, body, (int)body_len,
                                         APP_MQTT_DP_QOS, 0, APP_MQTT_DP_STORE);
        bool ok = (rc > 0);   /* enqueue 返回 message id;0/-1/-2 都不是成功 */

        if (!ok) {
            app_mqtt_dp_note_drop("enqueue", len, esp_mqtt_client_get_outbox_size(client), rc);
        }
        app_mqtt_client_release();
        return ok;
    }
}

static bool app_mqtt_dp_flush(void)
{
    uint8_t buf[APP_MQTT_DP_AGG_BYTES];
    size_t n;

    portENTER_CRITICAL(&s_dp_agg_mux);
    n = s_dp_agg_len;
    if (n > 0) {
        memcpy(buf, s_dp_agg, n);
        s_dp_agg_len = 0;
    }
    portEXIT_CRITICAL(&s_dp_agg_mux);
    if (n == 0) {
        return true;
    }
    {
        bool ok = app_mqtt_dp_send(buf, n);

        if (!ok) {
            s_dp_flush_fail_bytes += (uint32_t)n;
            if ((s_dp_flush_fail_bytes % 2048u) < (uint32_t)n) {
                ESP_LOGW(TAG, "dataplane flush failed (%uB), total=%uB",
                         (unsigned)n, (unsigned)s_dp_flush_fail_bytes);
            }
        }
        return ok;
    }
}

static bool app_mqtt_dataplane_write(const uint8_t *data, size_t len, void *ctx)
{
    bool ok = true;
    size_t off = 0;

    (void)ctx;
    if (data == NULL || len == 0 || len > 0x7FFFFFFFu) {
        return false;
    }
    while (off < len) {
        size_t space;
        size_t n;
        bool full;

        portENTER_CRITICAL(&s_dp_agg_mux);
        space = sizeof(s_dp_agg) - s_dp_agg_len;
        n = len - off;
        if (n > space) {
            n = space;
        }
        memcpy(s_dp_agg + s_dp_agg_len, data + off, n);
        s_dp_agg_len += n;
        off += n;
        full = (s_dp_agg_len == sizeof(s_dp_agg));
        portEXIT_CRITICAL(&s_dp_agg_mux);
        if (full) {
            ok = app_mqtt_dp_flush() && ok;
        }
    }
    return ok;
}

static void app_mqtt_dp_flush_task(void *arg)
{
    (void)arg;
    for (;;) {
        vTaskDelay(pdMS_TO_TICKS(APP_MQTT_DP_FLUSH_PERIOD_MS));
        (void)app_mqtt_dp_flush();
    }
}

static void app_mqtt_ensure_dp_flush_task(void)
{
    static bool s_started = false;

    if (s_started) {
        return;
    }
    if (xTaskCreate(app_mqtt_dp_flush_task, "mqtt_dp_flush", 2560, NULL, 4, NULL) != pdPASS) {
        ESP_LOGW(TAG, "mqtt_dp_flush task create failed");
        return;
    }
    s_started = true;
}

static void app_mqtt_request_initial_attributes(void)
{
    /* 1.3-57: 必须显式列出 sharedKeys。TB 对空对象 {} 的返回语义未定义
     * (实测只回 client 组),离线期间改的共享属性重连后同步不到。
     * 只取数据面 data;继电器/LED 只允许 RPC 控制。 */
    const char *payload = "{\"sharedKeys\":\"data\"}";
    char topic[64];
    esp_mqtt_client_handle_t client;

    if (!app_mqtt_connected_safe()) {
        return;
    }
    if (!app_mqtt_is_thingsboard_backend()) {
        return;
    }
    snprintf(topic, sizeof(topic), "v1/devices/me/attributes/request/%u",
             (unsigned)(xTaskGetTickCount() & 0xFFFFU));
    client = app_mqtt_client_safe();
    if (client != NULL) {
        esp_mqtt_client_publish(client, topic, payload, 0, 1, 0);
    }
    ESP_LOGI(TAG, "Requested initial attributes: %s", topic);
}

static void app_mqtt_publish_attributes(void)
{
    cJSON *root;
    char uart_frame[8];
    const esp_app_desc_t *app_desc;
    const char *fw_version;

    if (!app_mqtt_connected_safe()) {
        return;
    }
    if (app_mqtt_low_prio_defer()) {
        app_mqtt_on_io_change();
        return;
    }
    app_mqtt_build_uart_frame_str(uart_frame, sizeof(uart_frame));

    app_desc = esp_app_get_description();
    fw_version = (app_desc != NULL && app_desc->version[0] != '\0')
                    ? app_desc->version : "unknown";

    root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddStringToObject(root, "fw_version",  fw_version);
    cJSON_AddStringToObject(root, "net_mode",    app_network_mode_to_string(s_config.net_mode));
    cJSON_AddStringToObject(root, "ap_ssid",     s_config.ap_ssid);
    cJSON_AddStringToObject(root, "sta_ssid",    s_config.sta_ssid);
    cJSON_AddStringToObject(root, "sta_ip",      s_wifi.sta_ip);
    cJSON_AddStringToObject(root, "bt_mode",     app_bt_mode_to_string(s_config.bt_role));
    cJSON_AddStringToObject(root, "uart_frame",  uart_frame);
    cJSON_AddNumberToObject(root, "uart_baudrate", (double)s_config.uart_baudrate);
    {
        app_io_state_t io;

        app_io_get_state(&io);
        /* 用 *_state 命名,避免与共享属性命令键 relay_on/led_on 混淆。 */
        cJSON_AddBoolToObject(root, "relay_state", io.relay_on);
        cJSON_AddBoolToObject(root, "led_state", io.led_on);
        cJSON_AddBoolToObject(root, "input_state", io.input_active);
        cJSON_AddNumberToObject(root, "io_seq", io.seq);
        cJSON_AddStringToObject(root, "io_source", app_io_source_name(io.last_source));
        cJSON_AddNumberToObject(root, "relay_power_policy", s_config.relay_power_policy);
        cJSON_AddNumberToObject(root, "led_power_policy", s_config.led_power_policy);
    }
    app_mqtt_add_common_active_online(root);

    app_mqtt_publish_json(app_mqtt_attributes_topic(), root, false);
    cJSON_Delete(root);
}

static void app_mqtt_publish_state(void)
{
    cJSON *root;
    char uart_frame[8];

    if (!app_mqtt_connected_safe()) {
        return;
    }
    if (app_mqtt_low_prio_defer()) {
        app_mqtt_on_io_change();
        return;
    }
    app_mqtt_build_uart_frame_str(uart_frame, sizeof(uart_frame));

    root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    {
        app_io_state_t io;

        app_io_get_state(&io);
        cJSON_AddBoolToObject  (root, "relay_on",      io.relay_on);
        cJSON_AddBoolToObject  (root, "led_on",        io.led_on);
        cJSON_AddBoolToObject  (root, "input_active",  io.input_active);
        cJSON_AddNumberToObject(root, "io_seq",        io.seq);
        cJSON_AddStringToObject(root, "io_source",     app_io_source_name(io.last_source));
    }
    cJSON_AddStringToObject(root, "bt_mode",       app_bt_mode_to_string(s_config.bt_role));
    cJSON_AddStringToObject(root, "net_mode",      app_network_mode_to_string(s_config.net_mode));
    cJSON_AddBoolToObject  (root, "sta_has_ip",    s_wifi.sta_has_ip);
    cJSON_AddNumberToObject(root, "free_heap",     (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uart_baudrate", (double)s_config.uart_baudrate);
    cJSON_AddStringToObject(root, "uart_frame",    uart_frame);
    app_mqtt_add_common_active_online(root);

    app_mqtt_publish_json(app_mqtt_telemetry_topic(), root, true);
    cJSON_Delete(root);
}

/* ---------------------------------------------------------------------------
 * 1.3-31/1.3-32 命令面:METHOD 分发与统一层执行已抽到命令内核 cmd.c
 * (MQTT RPC 与 BLE CMD 特征共用同一分发器)。本函数只承担 MQTT transport:
 * 主题判定、RPC 限频(仅 RPC 主题,属性消息不占槽)、响应发布、先回包后重启。
 * ------------------------------------------------------------------------- */
static int64_t s_last_cmd_ms = 0;

static void app_mqtt_apply_command(const char *topic, const char *payload, int data_len)
{
    const char *rpc_prefix = "v1/devices/me/rpc/request/";
    const char *custom_rpc_topic = "iothub/rpc";
    bool is_rpc = (topic != NULL && strncmp(topic, rpc_prefix, strlen(rpc_prefix)) == 0);
    bool is_custom_rpc = (topic != NULL && strcmp(topic, custom_rpc_topic) == 0);
    bool wants_response = is_rpc || is_custom_rpc;
    char response_topic[128];
    cJSON *root = cJSON_ParseWithLength(payload, data_len < 0 ? (payload ? strlen(payload) : 0) : (size_t)data_len);

    if (root == NULL) {
        int preview_len;
        if (data_len < 0) {
            preview_len = payload ? (int)strlen(payload) : 0;
        } else {
            preview_len = data_len;
        }
        if (preview_len > 96) {
            preview_len = 96;
        }
        ESP_LOGW(TAG, "JSON parse failed, len=%d payload_preview='%.*s'",
                 data_len, preview_len, payload ? payload : "");
        return;
    }

    if (is_rpc) {
        snprintf(response_topic, sizeof(response_topic),
                 "v1/devices/me/rpc/response/%s", topic + strlen(rpc_prefix));
    } else if (is_custom_rpc) {
        snprintf(response_topic, sizeof(response_topic), "%s/resp", custom_rpc_topic);
    }

    /* 限频:5 次/秒(2026-10-15 用户决策:TB 界面连点会命中 1 次/秒,放宽仍可挡洪水)
     * —— 只对 RPC 主题生效;attributes 响应等其它入站消息不占用也不触发限频。 */
    if (is_rpc || is_custom_rpc) {
        int64_t now_ms = esp_timer_get_time() / 1000;

        if (now_ms - s_last_cmd_ms < 200) {
            cJSON *resp = cJSON_CreateObject();

            ESP_LOGW(TAG, "cloud cmd rate limited (last=%lldms)", (long long)s_last_cmd_ms);
            if (resp != NULL) {
                esp_mqtt_client_handle_t client;

                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "error", "rate_limited");
                client = app_mqtt_client_safe();
                if (client != NULL) {
                    char *body = cJSON_PrintUnformatted(resp);
                    if (body != NULL) {
                        ESP_LOGI(TAG, "rpc resp -> %s: %.256s", response_topic, body);
                        esp_mqtt_client_publish(client, response_topic, body, 0, 1, 0);
                        cJSON_free(body);
                    }
                }
                cJSON_Delete(resp);
            }
            cJSON_Delete(root);
            return;
        }
        s_last_cmd_ms = now_ms;
    }

    {
        app_cmd_result_t out = {0};
        const char *err = NULL;
        cJSON *resp = NULL;

        if (app_cmd_execute_ex(root, APP_CMD_SOURCE_MQTT, &resp, &out, &err) != ESP_OK || resp == NULL) {
            resp = cJSON_CreateObject();
            if (resp != NULL) {
                cJSON_AddBoolToObject(resp, "success", false);
                cJSON_AddStringToObject(resp, "error", err != NULL ? err : "internal_error");
            }
        }
        if (wants_response && resp != NULL) {
            esp_mqtt_client_handle_t client = app_mqtt_client_safe();

            if (client != NULL) {
                char *resp_body = cJSON_PrintUnformatted(resp);

                if (resp_body != NULL) {
                    ESP_LOGI(TAG, "rpc resp -> %s: %.256s", response_topic, resp_body);
                    esp_mqtt_client_publish(client, response_topic, resp_body, 0, 1, 0);
                    cJSON_free(resp_body);
                }
            }
        }
        if (resp != NULL) {
            cJSON_Delete(resp);
        }
        if (out.reboot_after_response) {
            app_reboot_schedule("cloud_cmd_reboot");
            ESP_LOGI(TAG, "cloud cmd effect=reboot: response sent, reboot scheduled (1.5s)");
        }
        cJSON_Delete(root);

        if (out.handled && out.changed_any) {
            app_mqtt_publish_attributes();
        }
        app_mqtt_publish_state();
    }
}

/* 1.3-57: TB 共享属性统一入口。两路来源:
 *   实时更新  v1/devices/me/attributes            → 扁平 JSON
 *   初始同步  v1/devices/me/attributes/response/<id> → {"client":{...},"shared":{...}}
 * 只处理数据面 data(base64 → data_pipe);继电器/LED 只允许 RPC 命令,
 * 共享属性中的 relay_on/led_on 已删除,避免平台同时存在两套控制入口。 */
static void app_mqtt_apply_shared_attrs(const cJSON *shared)
{
    const cJSON *data_item;

    if (!cJSON_IsObject(shared)) {
        return;
    }
    data_item = cJSON_GetObjectItemCaseSensitive(shared, "data");
    if (cJSON_IsString(data_item) && data_item->valuestring != NULL) {
        size_t olen = 0;
        int rc = mbedtls_base64_decode(s_down_decode_buf, sizeof(s_down_decode_buf), &olen,
                                       (const unsigned char *)data_item->valuestring,
                                       strlen(data_item->valuestring));

        if (rc == 0 && olen > 0) {
            ESP_LOGI(TAG, "shared data -> pipe: %u bytes", (unsigned)olen);
            data_pipe_feed(DATA_PORT_MQTT, s_down_decode_buf, olen);
        } else {
            ESP_LOGW(TAG, "downlink base64 decode failed (rc=%d)", rc);
        }
    }
}

static bool app_mqtt_topic_is_tb_attributes(const char *topic, int topic_len)
{
    static const char exact[] = "v1/devices/me/attributes";
    static const char prefix[] = "v1/devices/me/attributes/response/";

    if (topic_len == (int)(sizeof(exact) - 1) &&
        strncmp(topic, exact, sizeof(exact) - 1) == 0) {
        return true;
    }
    return topic_len >= (int)(sizeof(prefix) - 1) &&
           strncmp(topic, prefix, sizeof(prefix) - 1) == 0;
}

static void app_mqtt_republish_boost_task(void *arg)
{
    (void)arg;
    for (;;) {
        BaseType_t notified;
        TickType_t deadline;
        EventBits_t stop_bits;

        notified = xTaskNotifyWaitIndexed(0, 0, ULONG_MAX, NULL, portMAX_DELAY);
        if (notified == pdPASS) {
            deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
        } else {
            continue;
        }

        for (;;) {
            TickType_t now = xTaskGetTickCount();
            TickType_t remaining;
            stop_bits = xEventGroupGetBits(s_mqtt.stop_events);
            if ((stop_bits & MQTT_STOP_EVENT_BOOST) != 0) {
                ESP_LOGI(TAG, "Boost task exiting on stop event");
                xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_BOOST);
                app_mqtt_lock();
                s_mqtt.boost_task = NULL;
                app_mqtt_unlock();
                vTaskDelete(NULL);
                return;
            }
            if (now >= deadline) {
                break;
            }
            remaining = deadline - now;
            notified = xTaskNotifyWaitIndexed(0, 0, ULONG_MAX, NULL, remaining);
            if (notified == pdPASS) {
                deadline = xTaskGetTickCount() + pdMS_TO_TICKS(3000);
            }
        }

        stop_bits = xEventGroupGetBits(s_mqtt.stop_events);
        if ((stop_bits & MQTT_STOP_EVENT_BOOST) != 0) {
            ESP_LOGI(TAG, "Boost task exiting on stop event");
            xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_BOOST);
            app_mqtt_lock();
            s_mqtt.boost_task = NULL;
            app_mqtt_unlock();
            vTaskDelete(NULL);
            return;
        }
        if (app_mqtt_connected_safe()) {
            ESP_LOGI(TAG, "3s boost: re-publish attributes + state to ensure liveness");
            app_mqtt_publish_attributes();
            app_mqtt_publish_state();
        }
    }
}

static bool app_mqtt_schedule_boost(void)
{
    TaskHandle_t h;
    BaseType_t rt;
    app_mqtt_ensure_runtime_init();
    app_mqtt_lock();
    h = (TaskHandle_t)s_mqtt.boost_task;
    if (h != NULL) {
        app_mqtt_unlock();
        xTaskNotifyGive(h);
        return true;
    }
    app_mqtt_unlock();
    xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_BOOST);
    rt = xTaskCreate(app_mqtt_republish_boost_task, "mqtt_boost", 2560, NULL, 4, &h);
    if (rt != pdPASS) {
        ESP_LOGW(TAG, "Failed to create mqtt_boost task");
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "boost_task_failed");
        return false;
    }
    app_mqtt_lock();
    if (s_mqtt.boost_task == NULL) {
        s_mqtt.boost_task = h;
    }
    app_mqtt_unlock();
    xTaskNotifyGive(h);
    return true;
}

/* ---- 问题与版本优化.md 1.3-10:应用层重连退避 ----
 * 关闭 esp-mqtt 内建固定 5s 自动重连后,由 mqtt_restart 任务按指数退避重连:
 * 连接正常时的配置变更仅等 200ms(step=0);连续失败 1s→2→4→8→16→30s。
 * s_restart_backoff_step 仅该任务自增、由 MQTT_EVENT_CONNECTED 复位(单写者,无需加锁)。 */
#define APP_MQTT_BACKOFF_MAX_STEP 6
static uint8_t s_restart_backoff_step = 0;

/* G3 operation 冲突协调：只延迟客户端重建，不延迟连接状态记录/stop。 */
#define APP_MQTT_RESTART_DEFER_MAX_MS 15000
static bool s_restart_deferred = false;
static bool s_restart_pending = false;
static uint8_t s_restart_defer_depth = 0;
static bool s_restart_active = false;
static esp_timer_handle_t s_restart_defer_timer = NULL;

static void app_mqtt_defer_timeout(void *arg);

static bool app_mqtt_defer_timer_ensure(void)
{
    const esp_timer_create_args_t args = {
        .callback = app_mqtt_defer_timeout,
        .name = "mqtt_defer",
    };
    esp_err_t err;

    if (s_restart_defer_timer != NULL) {
        return true;
    }
    err = esp_timer_create(&args, &s_restart_defer_timer);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "defer timer create failed: %s", esp_err_to_name(err));
        s_restart_defer_timer = NULL;
        return false;
    }
    return true;
}

static void app_mqtt_defer_timeout(void *arg)
{
    bool pending = false;

    (void)arg;
    app_mqtt_lock();
    if (s_restart_deferred) {
        s_restart_deferred = false;
        s_restart_defer_depth = 0;
        pending = s_restart_pending;
        s_restart_pending = false;
    }
    app_mqtt_unlock();
    if (pending) {
        ESP_LOGW(TAG, "restart defer timeout: release pending restart");
        (void)app_mqtt_schedule_restart();
    }
}

bool app_mqtt_restart_is_deferred(void)
{
    bool deferred;

    app_mqtt_lock();
    deferred = s_restart_deferred;
    app_mqtt_unlock();
    return deferred;
}

bool app_mqtt_restart_is_pending(void)
{
    bool pending;

    app_mqtt_lock();
    pending = s_restart_pending;
    app_mqtt_unlock();
    return pending;
}

esp_err_t app_mqtt_defer_restart_begin(void)
{
    bool first = false;

    if (!s_mqtt.started) {
        return ESP_OK;
    }
    if (!app_mqtt_defer_timer_ensure()) {
        return ESP_ERR_NO_MEM;
    }

    app_mqtt_lock();
    if (s_restart_active) {
        app_mqtt_unlock();
        return ESP_ERR_INVALID_STATE;   /* 已有 rebuild 在跑，operation 需稍后重试 */
    }
    s_restart_defer_depth++;
    if (s_restart_defer_depth == 1) {
        s_restart_deferred = true;
        first = true;
    }
    app_mqtt_unlock();

    if (first) {
        esp_err_t err;

        (void)esp_timer_stop(s_restart_defer_timer);
        err = esp_timer_start_once(s_restart_defer_timer,
                                   (uint64_t)APP_MQTT_RESTART_DEFER_MAX_MS * 1000ULL);
        if (err != ESP_OK) {
            app_mqtt_lock();
            if (s_restart_defer_depth > 0) {
                s_restart_defer_depth--;
            }
            if (s_restart_defer_depth == 0) {
                s_restart_deferred = false;
            }
            app_mqtt_unlock();
            return err;
        }
        ESP_LOGI(TAG, "restart deferred (max %ums)", APP_MQTT_RESTART_DEFER_MAX_MS);
    }
    return ESP_OK;
}

void app_mqtt_defer_restart_end(void)
{
    bool last = false;
    bool pending = false;

    app_mqtt_lock();
    if (s_restart_defer_depth > 0) {
        s_restart_defer_depth--;
    }
    if (s_restart_defer_depth == 0 && s_restart_deferred) {
        s_restart_deferred = false;
        last = true;
        pending = s_restart_pending;
        s_restart_pending = false;
    }
    app_mqtt_unlock();

    if (last) {
        if (s_restart_defer_timer != NULL) {
            (void)esp_timer_stop(s_restart_defer_timer);
        }
        ESP_LOGI(TAG, "restart defer end pending=%d", (int)pending);
        if (pending) {
            (void)app_mqtt_schedule_restart();
        }
    }
}

static uint32_t app_mqtt_backoff_delay_ms(uint8_t step)
{
    /* 1.3-36 调整:原 {200ms,1s,2s,4s,8s,16s,30s} 在 broker 不可达时前 2 分钟
     * 内连发 6+ 次连接尝试,每次尝试 5s 内把 free 堆压到 ~2KB,正好砸在用户
     * 开机后打开配置页的时段(页面 503/半截)。起步提到 2s(配置变更后 2s
     * 内仍会重连),随后拉开到 5s/10s/20s/30s,给堆留出恢复窗口。 */
    static const uint32_t table[] = { 2000, 5000, 10000, 20000, 30000, 30000, 30000 };

    if (step >= sizeof(table) / sizeof(table[0])) {
        step = (uint8_t)(sizeof(table) / sizeof(table[0]) - 1);
    }
    return table[step];
}

static void app_mqtt_event_handler(void *handler_args, esp_event_base_t base, int32_t event_id, void *event_data)
{
    esp_mqtt_event_handle_t event = event_data;
    char topic[96];
    static char payload[APP_JSON_BUFFER_SIZE];
    int copy_len;

    (void)handler_args;
    (void)base;

    switch ((esp_mqtt_event_id_t)event_id) {
    case MQTT_EVENT_CONNECTED:
        s_mqtt_connecting = false;
        app_mqtt_lock();
        s_mqtt.connected = true;
        s_mqtt.fatal_error = false;
        app_mqtt_unlock();
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "");
        s_restart_backoff_step = 0;   /* 连上即复位退避(问题与版本优化.md 1.3-10) */
        esp_mqtt_client_subscribe(event->client, app_mqtt_rpc_topic(), 1);
        esp_mqtt_client_subscribe(event->client, app_mqtt_shared_attr_response_topic(), 1);
        if (app_mqtt_is_thingsboard_backend()) {
            /* 方案 A:下行 = TB shared attributes 更新(同时解锁"属性即配置"通道) */
            esp_mqtt_client_subscribe(event->client, "v1/devices/me/attributes", 1);
        } else if (s_data_down_topic[0] != '\0') {
            esp_mqtt_client_subscribe(event->client, s_data_down_topic, 1);
        }
        app_mqtt_publish_attributes();
        app_mqtt_publish_state();
        app_mqtt_request_initial_attributes();
        ESP_LOGI(TAG, "MQTT connected: subscribed RPC + shared attrs, requested initial attrs");
        app_mqtt_schedule_boost();
        break;
    case MQTT_EVENT_DISCONNECTED: {
        bool is_current_client;

        s_mqtt_connecting = false;
        app_mqtt_lock();
        s_mqtt.connected = false;
        /* 重连流程会先把 s_mqtt.client 置 NULL 再 stop/destroy 旧客户端；
         * 旧客户端随后补发的 DISCONNECTED 不能再次触发 restart，否则
         * "stop 旧客户端 -> 收旧事件 -> 调度重连 -> 再 stop" 会形成自激循环。 */
        is_current_client = (event != NULL && event->client == s_mqtt.client);
        app_mqtt_unlock();
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "disconnected");
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED%s",
                 is_current_client ? "" : " (stale client, ignored)");
        /* 已关闭客户端内建自动重连 → 由应用层按退避重连(问题与版本优化.md 1.3-10)。
         * 仅在 STA 仍有 IP 时才调度:STA 掉线的情况由 WIFI_EVENT 处理器统一处理,
         * 避免"网络已断还在猛重连"的风暴。 */
        if (is_current_client && s_wifi.sta_has_ip) {
            (void)app_mqtt_schedule_restart();
        }
        break;
    }
    case MQTT_EVENT_PUBLISHED:
        app_mqtt_lock();
        s_mqtt.last_msg_id = event->msg_id;
        ESP_LOGI(TAG, "MQTT_EVENT_PUBLISHED: broker ACK msg_id=%d total_published=%u",
                 event->msg_id, (unsigned)s_mqtt.publish_count);
        app_mqtt_unlock();
        break;
    case MQTT_EVENT_DATA:
        copy_len = event->topic_len < (int)sizeof(topic) - 1 ? event->topic_len : (int)sizeof(topic) - 1;
        memcpy(topic, event->topic, copy_len);
        topic[copy_len] = '\0';
        copy_len = event->data_len < (int)sizeof(payload) - 1 ? event->data_len : (int)sizeof(payload) - 1;
        memcpy(payload, event->data, copy_len);
        payload[copy_len] = '\0';
        {
            int preview_cap = strstr(topic, "attributes") != NULL ? 256 : 96;

            if (event->data_len < preview_cap) {
                preview_cap = event->data_len;
            }
            ESP_LOGI(TAG, "MQTT RX topic='%s' len=%d data_preview='%.*s'",
                     topic, event->data_len, preview_cap, payload);
        }
        /* 数据/配置下行(1.3-07 方案 A,1.3-57 补初始同步):
         * TB: v1/devices/me/attributes = 共享属性变更(扁平 JSON);
         *     v1/devices/me/attributes/response/<id> = 初始同步应答
         *     ({"client":{...},"shared":{...}})。只取 shared.data(base64)
         *     喂管道;GPIO 控制只走 RPC,已不再接受共享属性 relay_on/led_on。
         * custom: iothub/{mac}/data/down 原始字节。命令帧永不进管道。 */
        if (app_mqtt_is_thingsboard_backend() &&
            app_mqtt_topic_is_tb_attributes(topic, event->topic_len)) {
            cJSON *root = cJSON_ParseWithLength(payload, copy_len);

            if (cJSON_IsObject(root)) {
                const cJSON *wrapped = cJSON_GetObjectItemCaseSensitive(root, "shared");

                /* 实时更新是扁平对象;初始应答多包一层 shared */
                app_mqtt_apply_shared_attrs(cJSON_IsObject(wrapped) ? wrapped : root);
            }
            if (root != NULL) {
                cJSON_Delete(root);
            }
            break;
        }
        if (s_data_down_topic[0] != '\0' &&
            event->topic_len == (int)strlen(s_data_down_topic) &&
            strncmp(topic, s_data_down_topic, (size_t)event->topic_len) == 0) {
            if (event->data_len > 0) {
                data_pipe_feed(DATA_PORT_MQTT, (const uint8_t *)event->data, (size_t)event->data_len);
            }
            break;
        }
        app_mqtt_apply_command(topic, payload, event->data_len);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED: msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED: msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        s_mqtt_connecting = false;
        app_mqtt_lock();
        s_mqtt.connected = false;
        app_mqtt_unlock();
        /* 1.3-43:认证被拒/标识被拒属致命错误(换 token 前重连必失败),
         * 置位后自动重连停止,避免对 broker 无限 CONNECT;配置变更会清位 */
        if (event->error_handle != NULL &&
            event->error_handle->error_type == MQTT_ERROR_TYPE_CONNECTION_REFUSED &&
            (event->error_handle->connect_return_code == 2 ||
             event->error_handle->connect_return_code == 4 ||
             event->error_handle->connect_return_code == 5)) {
            app_mqtt_lock();
            s_mqtt.fatal_error = true;
            app_mqtt_unlock();
        }
        if (event->error_handle != NULL) {
            snprintf(s_mqtt.last_error, sizeof(s_mqtt.last_error),
                     "mqtt_err_type=%d connect_rc=%d",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code);
            ESP_LOGE(TAG, "MQTT_EVENT_ERROR: type=%d connect_rc=%d",
                     (int)event->error_handle->error_type,
                     (int)event->error_handle->connect_return_code);
        } else {
            snprintf(s_mqtt.last_error, sizeof(s_mqtt.last_error), "event_error_%ld", (long)event_id);
        }
        break;
    default:
        ESP_LOGD(TAG, "MQTT event id=%ld", (long)event_id);
        break;
    }
}

/* 等待任务确认退出:任务收到停止位后会清位、自清句柄再删除自己,
 * 这里轮询停止位被清即认为该任务已不再触碰 client。 */
static bool app_mqtt_wait_task_exit(EventBits_t stop_bit, TickType_t timeout_ticks)
{
    TickType_t deadline = xTaskGetTickCount() + timeout_ticks;

    while ((xEventGroupGetBits(s_mqtt.stop_events) & stop_bit) != 0) {
        if ((int32_t)(xTaskGetTickCount() - deadline) >= 0) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(20));
    }
    return true;
}

/* 停止客户端并回收 telemetry/boost 任务(不含 restart 任务,供 apply_config 重建流程使用,
 * 避免重启流程等待自己)。所有任务确认退出后才销毁 client;若任务超时未退出,
 * 则只停不销毁(有界泄漏一个 client),避免其仍持有 client 指针造成悬空引用。 */
static void app_mqtt_stop_clients(void)
{
    esp_mqtt_client_handle_t client_to_destroy = NULL;
    TaskHandle_t boost_task_to_notify = NULL;
    bool has_telemetry;
    bool has_boost;
    bool tasks_exited = true;

    app_mqtt_ensure_runtime_init();

    app_mqtt_lock();
    has_telemetry = (s_mqtt.telemetry_task != NULL);
    has_boost = (s_mqtt.boost_task != NULL);
    if (has_telemetry) {
        xEventGroupSetBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY);
    }
    if (has_boost) {
        boost_task_to_notify = (TaskHandle_t)s_mqtt.boost_task;
        xEventGroupSetBits(s_mqtt.stop_events, MQTT_STOP_EVENT_BOOST);
    }
    client_to_destroy = s_mqtt.client;
    s_mqtt.client = NULL;
    s_mqtt.connected = false;
    s_mqtt_connecting = false;
    app_mqtt_unlock();

    if (boost_task_to_notify != NULL) {
        xTaskNotifyGive(boost_task_to_notify);
    }
    if (has_telemetry) {
        tasks_exited &= app_mqtt_wait_task_exit(MQTT_STOP_EVENT_TELEMETRY, pdMS_TO_TICKS(12000));
    }
    if (has_boost) {
        tasks_exited &= app_mqtt_wait_task_exit(MQTT_STOP_EVENT_BOOST, pdMS_TO_TICKS(4000));
    }

    if (client_to_destroy == NULL) {
        return;
    }
    if (!tasks_exited) {
        ESP_LOGE(TAG, "mqtt tasks did not exit in time, skip client destroy (one client leaked)");
        esp_mqtt_client_stop(client_to_destroy);
        return;
    }
    /* 1.3-53:等所有 client 借用归还后再销毁 —— data_pipe/diag 等在途使用
     * 会解引用已释放句柄(板上 coredump:enqueue+64 LoadProhibited)。
     * s_mqtt.client 已置 NULL,不会再新增借用;在途借用都是短操作,轮询有界。 */
    for (;;) {
        int users;

        app_mqtt_lock();
        users = s_mqtt_client_users;
        app_mqtt_unlock();
        if (users == 0) {
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(5));
    }
    esp_mqtt_client_stop(client_to_destroy);
    esp_mqtt_client_destroy(client_to_destroy);
}

void app_mqtt_stop(void)
{
    TaskHandle_t restart_task_to_notify = NULL;
    bool has_restart;

    app_mqtt_ensure_runtime_init();

    app_mqtt_lock();
    has_restart = (s_mqtt.restart_task != NULL);
    if (has_restart) {
        restart_task_to_notify = (TaskHandle_t)s_mqtt.restart_task;
        xEventGroupSetBits(s_mqtt.stop_events, MQTT_STOP_EVENT_RESTART);
    }
    app_mqtt_unlock();
    if (restart_task_to_notify != NULL) {
        xTaskNotifyGive(restart_task_to_notify);
        app_mqtt_wait_task_exit(MQTT_STOP_EVENT_RESTART, pdMS_TO_TICKS(4000));
    }

    app_mqtt_stop_clients();
}

static void app_mqtt_telemetry_task(void *arg);

/* telemetry 任务是常驻单例,被 stop 流程退出后由本函数补建:
 * 启动时和每次 apply_config 成功后调用,保证断线重连后周期遥测不消失。 */
static bool app_mqtt_ensure_telemetry_task(void)
{
    BaseType_t rt;
    TaskHandle_t handle = NULL;

    app_mqtt_ensure_runtime_init();
    app_mqtt_lock();
    if (s_mqtt.telemetry_task != NULL) {
        app_mqtt_unlock();
        return true;
    }
    app_mqtt_unlock();

    xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY);
    rt = xTaskCreate(app_mqtt_telemetry_task, "mqtt_telemetry", 3072, NULL, 5, &handle);
    if (rt != pdPASS) {
        ESP_LOGW(TAG, "Failed to create mqtt_telemetry task");
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "telemetry_task_failed");
        return false;
    }
    app_mqtt_lock();
    if (s_mqtt.telemetry_task == NULL) {
        s_mqtt.telemetry_task = handle;
    }
    app_mqtt_unlock();
    return true;
}

esp_err_t app_mqtt_apply_config(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};
    const char *will_payload = "{\"online\":false}";
    esp_mqtt_client_handle_t new_client = NULL;
    bool is_tb_backend;
    esp_err_t err;

    app_mqtt_ensure_runtime_init();
    app_mqtt_stop_clients();

    if (strlen(s_config.mqtt_host) == 0 || s_config.mqtt_port == 0) {
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "mqtt_not_configured");
        return ESP_OK;
    }
    if (!app_network_mode_has_sta(s_config.net_mode) || !s_wifi.sta_has_ip) {
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "waiting_sta_ip");
        return ESP_OK;
    }

    app_mqtt_build_uri();
    app_mqtt_build_client_id();
    app_mqtt_build_data_topics();
    /* 数据面 MQTT 端口(1.3-07):出站 = enqueue 到 up 主题;下行由订阅回调喂入 */
    data_pipe_register_port(DATA_PORT_MQTT, app_mqtt_dataplane_write, NULL);
    app_mqtt_ensure_dp_flush_task();   /* 1.3-76:256B 聚合的定时兜底 flush */

    is_tb_backend = strcmp(s_config.mqtt_backend, "tb_cloud") == 0 ||
                    strcmp(s_config.mqtt_backend, "tb_selfhost") == 0;

    mqtt_cfg.broker.address.uri = s_mqtt.uri;
    /* 问题与版本优化.md 1.3-10:keepalive 15→45s(减少 PINGREQ 与"无响应断连"概率,
     * 仍在 ThingsBoard 默认 60s 限制内);重连改由**应用层 mqtt_restart 任务**承担 ——
     * 关掉客户端内建的固定 5s 自动重连,改为**指数退避**(200ms→1→2→4→8→16→30s,
     * 连上即复位),避免 broker 不可达时的重连风暴占 socket/堆 */
    mqtt_cfg.session.keepalive = 45;
    /* 堆口径收紧(2026-10-15):BLE + 连接态 MQTT + 数据流三样常驻把 free 压到 ~2-7KB,
     * 页面/小接口随机失败。收 outbox 与任务栈:buffer 1024→512、out_size 512→256、
     * 共回收 ~768B 常驻;数据面另有 outbox 背压水线兜底。 */
    mqtt_cfg.buffer.size = 512;
    /* 1.3-76:数据面聚合 256B 后 TB JSON 约 354B,out_size=256 装不下会导致
     * 发送失败/客户端反复重连;恢复到 512(代价仅 +256B)。 */
    mqtt_cfg.buffer.out_size = 512;
    /* 1.3-76:官方 esp-mqtt 的 outbox.limit 是字节预算;给 QoS1 遥测/属性
     * 留 8KB;数据面 QoS1 仍走字节阈值(4KB),outbox 满则丢并计数。 */
    mqtt_cfg.outbox.limit = 8192;
    /* 1.3-43:非 TLS 任务栈 6144→4096,省 2KB 常驻堆(文章建议 4096;
     * TLS 保持 6144,mbedtls 握手栈需求)。此前曾随快路径实验一并回退,
     * 现按官方建议单独恢复。 */
    mqtt_cfg.task.stack_size = s_config.mqtt_use_tls ? 6144 : 4096;
    mqtt_cfg.session.disable_clean_session = false;
    mqtt_cfg.network.timeout_ms = 5000;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;   /* 已由 disable_auto_reconnect 接管,保留仅为兼容 */
    mqtt_cfg.network.disable_auto_reconnect = true;
    mqtt_cfg.credentials.client_id = s_mqtt.client_id;
    if (s_config.mqtt_use_tls) {
        mqtt_cfg.broker.verification.crt_bundle_attach = esp_crt_bundle_attach;
    }
    if (strlen(s_config.mqtt_token) > 0) {
        mqtt_cfg.credentials.username = s_config.mqtt_token;
    }

    if (is_tb_backend) {
        mqtt_cfg.session.last_will.topic = "v1/devices/me/attributes";
        mqtt_cfg.session.last_will.msg = will_payload;
        mqtt_cfg.session.last_will.msg_len = strlen(will_payload);
        mqtt_cfg.session.last_will.qos = 1;
        mqtt_cfg.session.last_will.retain = 0;
    }

    ESP_LOGI(TAG, "Starting MQTT: uri=%s, client_id=%s, token_len=%u, tls=%s, will=%s",
             s_mqtt.uri, s_mqtt.client_id, (unsigned)strlen(s_config.mqtt_token),
             s_config.mqtt_use_tls ? "yes" : "no",
             is_tb_backend ? "v1/devices/me/attributes" : "none");

    s_mqtt_connecting = true;
    new_client = esp_mqtt_client_init(&mqtt_cfg);
    if (new_client == NULL) {
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "mqtt_init_failed");
        return ESP_FAIL;
    }

    app_mqtt_lock();
    s_mqtt.client = new_client;
    app_mqtt_unlock();

    esp_mqtt_client_register_event(new_client, ESP_EVENT_ANY_ID, app_mqtt_event_handler, NULL);
    err = esp_mqtt_client_start(new_client);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mqtt start failed: %s", esp_err_to_name(err));
        app_mqtt_lock();
        if (s_mqtt.client == new_client) {
            s_mqtt.client = NULL;
        }
        s_mqtt.connected = false;
        app_mqtt_unlock();
        esp_mqtt_client_destroy(new_client);
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "mqtt_start_failed");
        return err;
    }
    app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "connecting");
    app_mqtt_ensure_telemetry_task();
    return ESP_OK;
}

static void app_mqtt_restart_task_fn(void *arg)
{
    (void)arg;
    for (;;) {
        BaseType_t notified;
        EventBits_t stop_bits;
        notified = xTaskNotifyWaitIndexed(0, 0, ULONG_MAX, NULL, portMAX_DELAY);
        if (notified != pdPASS) {
            continue;
        }
        stop_bits = xEventGroupGetBits(s_mqtt.stop_events);
        if ((stop_bits & MQTT_STOP_EVENT_RESTART) != 0) {
            ESP_LOGI(TAG, "Restart task exiting on stop event");
            xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_RESTART);
            app_mqtt_lock();
            s_mqtt.restart_task = NULL;
            app_mqtt_unlock();
            vTaskDelete(NULL);
            return;
        }
        /* 1.3-43:致命错误(认证被拒)期间不再自动重连,等待下一次"配置变更"
         * 触发的重启(变更路径先清位再调度,见 config_apply)。 */
        if (s_mqtt.fatal_error) {
            ESP_LOGW(TAG, "mqtt fatal error: auto reconnect paused (waiting config change)");
            continue;
        }
        /* 指数退避(问题与版本优化.md 1.3-10):连上后 step 复位为 0 → 配置变更类重启仅等 200ms;
         * 连续失败则 1s→2→4→8→16→30s,避免"断网/ broker 不可达"时的重连风暴 */
        vTaskDelay(pdMS_TO_TICKS(app_mqtt_backoff_delay_ms(s_restart_backoff_step)));
        if (s_restart_backoff_step < APP_MQTT_BACKOFF_MAX_STEP) {
            s_restart_backoff_step++;
        }
        stop_bits = xEventGroupGetBits(s_mqtt.stop_events);
        if ((stop_bits & MQTT_STOP_EVENT_RESTART) != 0) {
            ESP_LOGI(TAG, "Restart task exiting on stop event");
            xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_RESTART);
            app_mqtt_lock();
            s_mqtt.restart_task = NULL;
            app_mqtt_unlock();
            vTaskDelete(NULL);
            return;
        }
        if (app_mqtt_restart_is_deferred()) {
            app_mqtt_lock();
            s_restart_pending = true;
            app_mqtt_unlock();
            ESP_LOGI(TAG, "restart attempt deferred/merged");
            continue;
        }
        /* 1.3-44:内存深紧张时推迟连接尝试 —— esp-mqtt 建客户端 + TCP + 订阅
         * 一次要吃掉 ~6KB 堆;在 largest 已跌破页面发送门槛时硬连,会把水位
         * 砸穿并让页面/数据面连锁失败。有界等待(≤5s):水位恢复即走;持续
         * 紧张也只是"晚 5s 重试",由退避节奏兜底(不是无界阻塞)。 */
        {
            int guard;

            for (guard = 0; guard < 25 && app_mem_level() == APP_MEM_CRITICAL; guard++) {
                vTaskDelay(pdMS_TO_TICKS(200));
            }
            if (guard > 0) {
                ESP_LOGW(TAG, "mqtt restart deferred %dms (heap critical)", guard * 200);
            }
        }
        ESP_LOGW(TAG, "mqtt restart attempt (backoff step %u)", (unsigned)s_restart_backoff_step);
        app_mqtt_lock();
        s_restart_active = true;
        app_mqtt_unlock();
        if (app_mqtt_apply_config() != ESP_OK) {
            ESP_LOGE(TAG, "MQTT apply config failed");
        }
        app_mqtt_lock();
        s_restart_active = false;
        app_mqtt_unlock();
    }
}

bool app_mqtt_schedule_restart(void)
{
    TaskHandle_t h;
    BaseType_t rt;
    app_mqtt_ensure_runtime_init();
    app_mqtt_lock();
    if (s_restart_deferred) {
        s_restart_pending = true;
        app_mqtt_unlock();
        ESP_LOGI(TAG, "restart request deferred/merged");
        return true;
    }
    h = (TaskHandle_t)s_mqtt.restart_task;
    if (h != NULL) {
        app_mqtt_unlock();
        xTaskNotifyGive(h);
        return true;
    }
    app_mqtt_unlock();
    xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_RESTART);
    rt = xTaskCreate(app_mqtt_restart_task_fn, "mqtt_restart", 3072, NULL, 5, &h);
    if (rt != pdPASS) {
        ESP_LOGE(TAG, "Failed to create mqtt_restart task");
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "restart_task_failed");
        return false;
    }
    app_mqtt_lock();
    if (s_mqtt.restart_task == NULL) {
        s_mqtt.restart_task = h;
    }
    app_mqtt_unlock();
    xTaskNotifyGive(h);
    return true;
}

void app_mqtt_restart_task(void *arg)
{
    (void)arg;
    app_mqtt_schedule_restart();
}

void app_mqtt_on_io_change(void)
{
    if (s_mqtt.stop_events != NULL) {
        xEventGroupSetBits(s_mqtt.stop_events, MQTT_IO_EVENT);
    }
}

static void app_mqtt_telemetry_task(void *arg)
{
    EventBits_t bits;
    TickType_t wait_ticks = pdMS_TO_TICKS(10000);

    (void)arg;
    for (;;) {
        bits = xEventGroupWaitBits(s_mqtt.stop_events,
                                   MQTT_STOP_EVENT_TELEMETRY | MQTT_IO_EVENT,
                                   pdFALSE, pdFALSE, wait_ticks);
        if ((bits & MQTT_STOP_EVENT_TELEMETRY) != 0) {
            ESP_LOGI(TAG, "Telemetry task exiting on stop request");
            xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY);
            app_mqtt_lock();
            s_mqtt.telemetry_task = NULL;
            app_mqtt_unlock();
            vTaskDelete(NULL);
            return;
        }
        /* 1.3-89:页面发送/outbox 积压时只延后可合并的 state/attributes;
         * 保留 IO 事件,页面结束后补发最新值。RPC 响应不经过本任务。 */
        if (app_mqtt_low_prio_defer()) {
            app_mqtt_on_io_change();
            vTaskDelay(pdMS_TO_TICKS(500));
            continue;
        }
        /* 1.3-44:内存深紧张时本拍遥测让路 —— 发布路径(cJSON + outbox +
         * TCP 发送缓冲)是低堆期最容易失败并把压力放大的一环;跳过本拍
         * 不影响连接本体(心跳由 MQTT 客户端自己维持),水位恢复后下个
         * 周期照常。让路计数经 app_mqtt_tele_shed_count() 暴露给 diag。 */
        if ((bits & MQTT_IO_EVENT) != 0) {
            xEventGroupClearBits(s_mqtt.stop_events, MQTT_IO_EVENT);
        }
        if (app_mem_level() == APP_MEM_CRITICAL) {
            const size_t largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);

            s_tele_shed_count++;
            if (s_tele_shed_count <= 3u || (s_tele_shed_count % 20u) == 0u) {
                ESP_LOGW(TAG, "telemetry shed #%u (heap critical: free=%u largest=%u)",
                         (unsigned)s_tele_shed_count,
                         (unsigned)esp_get_free_heap_size(), (unsigned)largest);
            }
            continue;
        }
        app_mqtt_publish_state();
        if ((bits & MQTT_IO_EVENT) != 0) {
            app_mqtt_publish_attributes();
        }
    }
}

/* mqtt.c 自己监听系统网络事件并在 STA 断连/拿到 IP 时启停客户端,
 * wifi.c 不再反向调用 mqtt,两条模块之间只保留 mqtt→wifi 的单向状态读取。 */
static void app_mqtt_network_event_handler(void *handler_args, esp_event_base_t event_base,
                                           int32_t event_id, void *event_data)
{
    (void)handler_args;
    (void)event_data;

    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        ESP_LOGI(TAG, "STA disconnected, stopping MQTT client");
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "waiting_sta_ip");
        app_mqtt_stop();
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        if (!app_mqtt_schedule_restart()) {
            ESP_LOGE(TAG, "STA got IP: failed to schedule MQTT restart");
        }
    }
}

static void app_mqtt_register_network_events(void)
{
    static bool s_network_events_registered = false;
    esp_err_t err;

    if (s_network_events_registered) {
        return;
    }
    err = esp_event_handler_register(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     app_mqtt_network_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register WIFI_EVENT_STA_DISCONNECTED handler failed: %s", esp_err_to_name(err));
        return;
    }
    err = esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP,
                                     app_mqtt_network_event_handler, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "register IP_EVENT_STA_GOT_IP handler failed: %s", esp_err_to_name(err));
        esp_event_handler_unregister(WIFI_EVENT, WIFI_EVENT_STA_DISCONNECTED,
                                     app_mqtt_network_event_handler);
        return;
    }
    s_network_events_registered = true;
}

esp_err_t app_mqtt_start(void)
{
    app_mqtt_ensure_runtime_init();
    app_mqtt_register_network_events();

    app_mqtt_lock();
    s_mqtt.started = true;
    app_mqtt_unlock();

    /* telemetry 任务不再在此创建:apply_config 的停止流程会先退掉旧任务,
     * 成功路径统一由 app_mqtt_ensure_telemetry_task() 补建。 */
    return app_mqtt_apply_config();
}
