#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "cJSON.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_check.h"
#include "esp_crt_bundle.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_ota_ops.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "mqtt_client.h"

#include "common.h"
#include "config.h"
#include "constants.h"
#include "mqtt.h"
#include "state.h"
#include "uart.h"
#include "wifi.h"

static const char *TAG = "mqtt";
#define MQTT_STOP_EVENT_TELEMETRY BIT0
#define MQTT_STOP_EVENT_BOOST     BIT1
#define MQTT_STOP_EVENT_RESTART   BIT2

extern app_config_t s_config;
mqtt_runtime_t s_mqtt = {0};
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
    client = app_mqtt_client_safe();
    if (client == NULL) {
        cJSON_free(body);
        return -1;
    }
    app_mqtt_lock();
    msg_id = esp_mqtt_client_publish(client, topic, body, 0, 1, 0);
    if (msg_id >= 0) {
        s_mqtt.last_msg_id = msg_id;
        if (is_telemetry) {
            s_mqtt.publish_count++;
        }
        ESP_LOGD(TAG, "Published %s msg_id=%d len=%u count=%u",
                 is_telemetry ? "telemetry" : "attributes",
                 msg_id, (unsigned)strlen(body),
                 (unsigned)s_mqtt.publish_count);
    } else if (is_telemetry) {
        ESP_LOGW(TAG, "Publish telemetry FAILED, msg_id=%d", msg_id);
    }
    app_mqtt_unlock();
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

static void app_mqtt_request_initial_attributes(void)
{
    const char *payload = "{}";
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
    cJSON_AddStringToObject(root, "bt_mode",     app_bt_mode_to_string(s_config.bt_mode));
    cJSON_AddStringToObject(root, "uart_frame",  uart_frame);
    cJSON_AddNumberToObject(root, "uart_baudrate", (double)s_config.uart_baudrate);
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
    app_mqtt_build_uart_frame_str(uart_frame, sizeof(uart_frame));

    root = cJSON_CreateObject();
    if (root == NULL) {
        return;
    }
    cJSON_AddBoolToObject  (root, "relay_on",      s_config.relay_on);
    cJSON_AddBoolToObject  (root, "led_on",        s_config.led_on);
    cJSON_AddBoolToObject  (root, "input_active",  app_get_input_state());
    cJSON_AddStringToObject(root, "bt_mode",       app_bt_mode_to_string(s_config.bt_mode));
    cJSON_AddStringToObject(root, "net_mode",      app_network_mode_to_string(s_config.net_mode));
    cJSON_AddBoolToObject  (root, "sta_has_ip",    s_wifi.sta_has_ip);
    cJSON_AddNumberToObject(root, "free_heap",     (double)esp_get_free_heap_size());
    cJSON_AddNumberToObject(root, "uart_baudrate", (double)s_config.uart_baudrate);
    cJSON_AddStringToObject(root, "uart_frame",    uart_frame);
    app_mqtt_add_common_active_online(root);

    app_mqtt_publish_json(app_mqtt_telemetry_topic(), root, true);
    cJSON_Delete(root);
}

static bool app_mqtt_parse_bool_param(const cJSON *params, bool fallback)
{
    const cJSON *p = NULL;
    if (params == NULL) {
        return fallback;
    }
    if (cJSON_IsObject(params)) {
        p = cJSON_GetObjectItemCaseSensitive(params, "value");
    }
    if (p == NULL) {
        p = params;
    }
    if (cJSON_IsBool(p)) {
        return cJSON_IsTrue(p);
    }
    if (cJSON_IsNumber(p)) {
        return p->valueint != 0;
    }
    return fallback;
}

static void app_mqtt_apply_command(const char *topic, const char *payload, int data_len)
{
    bool changed = false;
    bool handled = false;
    bool request_status = false;
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t new_relay_on;
    uint8_t new_led_on;
    uint8_t net_mode;
    char response_topic[128];
    const char *request_id = NULL;
    const char *rpc_prefix = "v1/devices/me/rpc/request/";
    cJSON *root = cJSON_ParseWithLength(payload, data_len < 0 ? (payload ? strlen(payload) : 0) : (size_t)data_len);

    app_config_lock();
    relay_on = s_config.relay_on;
    led_on = s_config.led_on;
    app_config_unlock();
    new_relay_on = relay_on;
    new_led_on = led_on;

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

    do {
        cJSON *item;
        cJSON *method;
        cJSON *params;

        item = cJSON_GetObjectItemCaseSensitive(root, "relay_on");
        if (cJSON_IsBool(item)) {
            bool v = cJSON_IsTrue(item);
            if (relay_on != v) {
                new_relay_on = v;
                changed = true;
            }
            handled = true;
        } else if (cJSON_IsNumber(item)) {
            bool v = item->valueint != 0;
            if (relay_on != v) {
                new_relay_on = v;
                changed = true;
            }
            handled = true;
        }

        item = cJSON_GetObjectItemCaseSensitive(root, "led_on");
        if (cJSON_IsBool(item)) {
            bool v = cJSON_IsTrue(item);
            if (led_on != v) {
                new_led_on = v;
                changed = true;
            }
            handled = true;
        } else if (cJSON_IsNumber(item)) {
            bool v = item->valueint != 0;
            if (led_on != v) {
                new_led_on = v;
                changed = true;
            }
            handled = true;
        }

        method = cJSON_GetObjectItemCaseSensitive(root, "method");
        params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (cJSON_IsString(method)) {
            const char *m = method->valuestring;
            if (strcmp(m, "setRelay") == 0) {
                bool v = app_mqtt_parse_bool_param(params, !relay_on);
                if (relay_on != v) {
                    new_relay_on = v;
                    changed = true;
                }
                handled = true;
            } else if (strcmp(m, "setLed") == 0) {
                bool v = app_mqtt_parse_bool_param(params, !led_on);
                if (led_on != v) {
                    new_led_on = v;
                    changed = true;
                }
                handled = true;
            } else if (strcmp(m, "getStatus") == 0) {
                handled = true;
                request_status = true;
            } else if (strcmp(m, "setState") == 0) {
                cJSON *rel = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "relay_on") : NULL;
                cJSON *led = cJSON_IsObject(params) ? cJSON_GetObjectItemCaseSensitive(params, "led_on") : NULL;
                if (cJSON_IsBool(rel) || cJSON_IsNumber(rel)) {
                    bool v = cJSON_IsBool(rel) ? cJSON_IsTrue(rel) : (rel->valueint != 0);
                    if (relay_on != v) { new_relay_on = v; changed = true; }
                }
                if (cJSON_IsBool(led) || cJSON_IsNumber(led)) {
                    bool v = cJSON_IsBool(led) ? cJSON_IsTrue(led) : (led->valueint != 0);
                    if (led_on != v) { new_led_on = v; changed = true; }
                }
                handled = true;
            }
        }
    } while (0);

    cJSON_Delete(root);

    if (changed) {
        app_config_lock();
        s_config.relay_on = new_relay_on;
        s_config.led_on = new_led_on;
        app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
        app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
        app_config_save(&s_config);
        app_config_unlock();
        relay_on = new_relay_on;
        led_on = new_led_on;
    }

    app_config_lock();
    net_mode = s_config.net_mode;
    app_config_unlock();

    if (handled && topic != NULL &&
        strncmp(topic, rpc_prefix, strlen(rpc_prefix)) == 0) {
        cJSON *resp = cJSON_CreateObject();
        if (resp == NULL) {
            goto publish_attributes_and_state;
        }
        request_id = topic + strlen(rpc_prefix);
        snprintf(response_topic, sizeof(response_topic),
                 "v1/devices/me/rpc/response/%s", request_id);
        if (request_status) {
            cJSON_AddBoolToObject  (resp, "relay_on",     relay_on);
            cJSON_AddBoolToObject  (resp, "led_on",       led_on);
            cJSON_AddBoolToObject  (resp, "input_active", app_get_input_state());
            cJSON_AddStringToObject(resp, "net_mode",     app_network_mode_to_string(net_mode));
            cJSON_AddBoolToObject  (resp, "sta_has_ip",   s_wifi.sta_has_ip);
            cJSON_AddStringToObject(resp, "sta_ip",       s_wifi.sta_ip);
        } else {
            cJSON_AddBoolToObject(resp, "success",  true);
            cJSON_AddBoolToObject(resp, "relay_on", relay_on);
            cJSON_AddBoolToObject(resp, "led_on",   led_on);
        }
        {
            esp_mqtt_client_handle_t client = app_mqtt_client_safe();
            if (client != NULL) {
                char *resp_body = cJSON_PrintUnformatted(resp);
                if (resp_body != NULL) {
                    esp_mqtt_client_publish(client, response_topic, resp_body, 0, 1, 0);
                    cJSON_free(resp_body);
                }
            }
        }
        cJSON_Delete(resp);
    }

publish_attributes_and_state:
    if (handled) {
        app_mqtt_publish_attributes();
    }
    app_mqtt_publish_state();
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
    rt = xTaskCreate(app_mqtt_republish_boost_task, "mqtt_boost", 3072, NULL, 4, &h);
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
        app_mqtt_lock();
        s_mqtt.connected = true;
        app_mqtt_unlock();
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "");
        esp_mqtt_client_subscribe(event->client, app_mqtt_rpc_topic(), 1);
        esp_mqtt_client_subscribe(event->client, app_mqtt_shared_attr_response_topic(), 1);
        app_mqtt_publish_attributes();
        app_mqtt_publish_state();
        app_mqtt_request_initial_attributes();
        ESP_LOGI(TAG, "MQTT connected: subscribed RPC + shared attrs, requested initial attrs");
        app_mqtt_schedule_boost();
        break;
    case MQTT_EVENT_DISCONNECTED:
        app_mqtt_lock();
        s_mqtt.connected = false;
        app_mqtt_unlock();
        app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "disconnected");
        ESP_LOGW(TAG, "MQTT_EVENT_DISCONNECTED");
        break;
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
        ESP_LOGI(TAG, "MQTT RX topic='%s' len=%d data_preview='%.*s'",
                 topic, event->data_len,
                 event->data_len < 96 ? event->data_len : 96,
                 payload);
        app_mqtt_apply_command(topic, payload, event->data_len);
        break;
    case MQTT_EVENT_SUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_SUBSCRIBED: msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_UNSUBSCRIBED:
        ESP_LOGI(TAG, "MQTT_EVENT_UNSUBSCRIBED: msg_id=%d", event->msg_id);
        break;
    case MQTT_EVENT_ERROR:
        app_mqtt_lock();
        s_mqtt.connected = false;
        app_mqtt_unlock();
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
    rt = xTaskCreate(app_mqtt_telemetry_task, "mqtt_telemetry", 4096, NULL, 5, &handle);
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

    is_tb_backend = strcmp(s_config.mqtt_backend, "tb_cloud") == 0 ||
                    strcmp(s_config.mqtt_backend, "tb_selfhost") == 0;

    mqtt_cfg.broker.address.uri = s_mqtt.uri;
    mqtt_cfg.session.keepalive = 15;
    mqtt_cfg.session.disable_clean_session = false;
    mqtt_cfg.network.timeout_ms = 5000;
    mqtt_cfg.network.reconnect_timeout_ms = 5000;
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
        vTaskDelay(pdMS_TO_TICKS(200));
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
        if (app_mqtt_apply_config() != ESP_OK) {
            ESP_LOGE(TAG, "MQTT apply config failed");
        }
    }
}

bool app_mqtt_schedule_restart(void)
{
    TaskHandle_t h;
    BaseType_t rt;
    app_mqtt_ensure_runtime_init();
    app_mqtt_lock();
    h = (TaskHandle_t)s_mqtt.restart_task;
    if (h != NULL) {
        app_mqtt_unlock();
        xTaskNotifyGive(h);
        return true;
    }
    app_mqtt_unlock();
    xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_RESTART);
    rt = xTaskCreate(app_mqtt_restart_task_fn, "mqtt_restart", 4096, NULL, 5, &h);
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

static void app_mqtt_telemetry_task(void *arg)
{
    EventBits_t bits;
    TickType_t wait_ticks = pdMS_TO_TICKS(10000);

    (void)arg;
    for (;;) {
        bits = xEventGroupWaitBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY,
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
        app_mqtt_publish_state();
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
