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
#include "esp_system.h"
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

extern app_config_t s_config;
mqtt_runtime_t s_mqtt = {0};

_Static_assert(sizeof(esp_mqtt_client_handle_t) == sizeof(((mqtt_runtime_t *)0)->client),
               "esp_mqtt_client_handle_t must be same size as opaque client pointer");
_Static_assert(sizeof(TaskHandle_t) == sizeof(((mqtt_runtime_t *)0)->telemetry_task),
               "TaskHandle_t must be same size as telemetry_task pointer");

static void app_mqtt_ensure_runtime_init(void)
{
    if (s_mqtt.lock == NULL) {
        s_mqtt.lock = xSemaphoreCreateMutex();
    }
    if (s_mqtt.stop_events == NULL) {
        s_mqtt.stop_events = xEventGroupCreate();
    }
}

static void app_mqtt_lock(void)
{
    app_mqtt_ensure_runtime_init();
    xSemaphoreTake(s_mqtt.lock, portMAX_DELAY);
}

static void app_mqtt_unlock(void)
{
    xSemaphoreGive(s_mqtt.lock);
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

static const char *app_mqtt_telemetry_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/telemetry";
    }
    return "iothub/telemetry";
}

static const char *app_mqtt_rpc_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/rpc/request/+";
    }
    return "iothub/rpc";
}

static const char *app_mqtt_attributes_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/attributes";
    }
    return "iothub/attributes";
}

static const char *app_mqtt_shared_attr_response_topic(void)
{
    if (strcmp(s_config.mqtt_backend, "tb_cloud") == 0 || strcmp(s_config.mqtt_backend, "tb_selfhost") == 0) {
        return "v1/devices/me/attributes/response/+";
    }
    return "iothub/attributes/response/+";
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

    if (strcmp(s_config.mqtt_backend, "tb_cloud") != 0 &&
        strcmp(s_config.mqtt_backend, "tb_selfhost") != 0) {
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
    char payload[512];
    char frame[8];
    esp_mqtt_client_handle_t client;

    if (!app_mqtt_connected_safe()) {
        return;
    }

    app_uart_build_frame(frame, sizeof(frame),
                         s_config.uart_data_bits,
                         (uart_parity_t)s_config.uart_parity_mode,
                         s_config.uart_stop_bits);
    snprintf(payload, sizeof(payload),
             "{\"fw_version\":\"phase4\",\"net_mode\":\"%s\",\"ap_ssid\":\"%s\","
             "\"sta_ssid\":\"%s\",\"sta_ip\":\"%s\",\"bt_mode\":\"%s\","
             "\"uart_frame\":\"%s\",\"uart_baudrate\":%u,"
             "\"active\":true,\"online\":true}",
             app_network_mode_to_string(s_config.net_mode),
             s_config.ap_ssid,
             s_config.sta_ssid,
             s_wifi.sta_ip,
             app_bt_mode_to_string(s_config.bt_mode),
             frame,
             (unsigned)s_config.uart_baudrate);

    client = app_mqtt_client_safe();
    if (client != NULL) {
        app_mqtt_lock();
        s_mqtt.last_msg_id = esp_mqtt_client_publish(client, app_mqtt_attributes_topic(),
                                                     payload, 0, 1, 0);
        if (s_mqtt.last_msg_id >= 0) {
            ESP_LOGD(TAG, "Published attributes msg_id=%d len=%u",
                     s_mqtt.last_msg_id, (unsigned)strlen(payload));
        }
        app_mqtt_unlock();
    }
}

static void app_mqtt_publish_state(void)
{
    char payload[512];
    char frame[8];
    esp_mqtt_client_handle_t client;

    if (!app_mqtt_connected_safe()) {
        return;
    }

    app_uart_build_frame(frame, sizeof(frame),
                         s_config.uart_data_bits,
                         (uart_parity_t)s_config.uart_parity_mode,
                         s_config.uart_stop_bits);
    snprintf(payload, sizeof(payload),
             "{\"relay_on\":%s,\"led_on\":%s,\"input_active\":%s,"
             "\"bt_mode\":\"%s\",\"net_mode\":\"%s\",\"sta_has_ip\":%s,"
             "\"free_heap\":%u,\"uart_baudrate\":%u,\"uart_frame\":\"%s\","
             "\"active\":true,\"online\":true}",
             s_config.relay_on ? "true" : "false",
             s_config.led_on ? "true" : "false",
             app_get_input_state() ? "true" : "false",
             app_bt_mode_to_string(s_config.bt_mode),
             app_network_mode_to_string(s_config.net_mode),
             s_wifi.sta_has_ip ? "true" : "false",
             (unsigned)esp_get_free_heap_size(),
             (unsigned)s_config.uart_baudrate,
             frame);

    client = app_mqtt_client_safe();
    if (client == NULL) {
        return;
    }
    app_mqtt_lock();
    s_mqtt.last_msg_id = esp_mqtt_client_publish(client, app_mqtt_telemetry_topic(),
                                                 payload, 0, 1, 0);
    if (s_mqtt.last_msg_id >= 0) {
        s_mqtt.publish_count++;
        ESP_LOGD(TAG, "Published telemetry msg_id=%d count=%u len=%u",
                 s_mqtt.last_msg_id, (unsigned)s_mqtt.publish_count,
                 (unsigned)strlen(payload));
    } else {
        ESP_LOGW(TAG, "Publish telemetry FAILED, last_msg_id=%d", s_mqtt.last_msg_id);
    }
    app_mqtt_unlock();
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
    char response_topic[128];
    char response_payload[256];
    const char *request_id = NULL;
    const char *rpc_prefix = "v1/devices/me/rpc/request/";
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

    do {
        cJSON *item;
        cJSON *method;
        cJSON *params;

        item = cJSON_GetObjectItemCaseSensitive(root, "relay_on");
        if (cJSON_IsBool(item)) {
            bool v = cJSON_IsTrue(item);
            if (s_config.relay_on != v) {
                s_config.relay_on = v;
                changed = true;
            }
            handled = true;
        } else if (cJSON_IsNumber(item)) {
            bool v = item->valueint != 0;
            if (s_config.relay_on != v) {
                s_config.relay_on = v;
                changed = true;
            }
            handled = true;
        }

        item = cJSON_GetObjectItemCaseSensitive(root, "led_on");
        if (cJSON_IsBool(item)) {
            bool v = cJSON_IsTrue(item);
            if (s_config.led_on != v) {
                s_config.led_on = v;
                changed = true;
            }
            handled = true;
        } else if (cJSON_IsNumber(item)) {
            bool v = item->valueint != 0;
            if (s_config.led_on != v) {
                s_config.led_on = v;
                changed = true;
            }
            handled = true;
        }

        method = cJSON_GetObjectItemCaseSensitive(root, "method");
        params = cJSON_GetObjectItemCaseSensitive(root, "params");
        if (cJSON_IsString(method)) {
            const char *m = method->valuestring;
            if (strcmp(m, "setRelay") == 0) {
                bool v = app_mqtt_parse_bool_param(params, !s_config.relay_on);
                if (s_config.relay_on != v) {
                    s_config.relay_on = v;
                    changed = true;
                }
                handled = true;
            } else if (strcmp(m, "setLed") == 0) {
                bool v = app_mqtt_parse_bool_param(params, !s_config.led_on);
                if (s_config.led_on != v) {
                    s_config.led_on = v;
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
                    if (s_config.relay_on != v) { s_config.relay_on = v; changed = true; }
                }
                if (cJSON_IsBool(led) || cJSON_IsNumber(led)) {
                    bool v = cJSON_IsBool(led) ? cJSON_IsTrue(led) : (led->valueint != 0);
                    if (s_config.led_on != v) { s_config.led_on = v; changed = true; }
                }
                handled = true;
            }
        }
    } while (0);

    cJSON_Delete(root);

    if (changed) {
        app_apply_output(app_relay_gpio(), s_config.relay_active_high, s_config.relay_on);
        app_apply_output(app_led_gpio(), s_config.led_active_high, s_config.led_on);
        app_config_save(&s_config);
    }

    if (handled && topic != NULL &&
        strncmp(topic, rpc_prefix, strlen(rpc_prefix)) == 0) {
        esp_mqtt_client_handle_t client;
        request_id = topic + strlen(rpc_prefix);
        snprintf(response_topic, sizeof(response_topic),
                 "v1/devices/me/rpc/response/%s", request_id);
        if (request_status) {
            snprintf(response_payload, sizeof(response_payload),
                     "{\"relay_on\":%s,\"led_on\":%s,\"input_active\":%s,"
                     "\"net_mode\":\"%s\",\"sta_has_ip\":%s,\"sta_ip\":\"%s\"}",
                     s_config.relay_on ? "true" : "false",
                     s_config.led_on ? "true" : "false",
                     app_get_input_state() ? "true" : "false",
                     app_network_mode_to_string(s_config.net_mode),
                     s_wifi.sta_has_ip ? "true" : "false",
                     s_wifi.sta_ip);
        } else {
            snprintf(response_payload, sizeof(response_payload),
                     "{\"success\":true,\"relay_on\":%s,\"led_on\":%s}",
                     s_config.relay_on ? "true" : "false",
                     s_config.led_on ? "true" : "false");
        }
        client = app_mqtt_client_safe();
        if (client != NULL) {
            esp_mqtt_client_publish(client, response_topic, response_payload, 0, 1, 0);
        }
    }

    if (handled) {
        app_mqtt_publish_attributes();
    }
    app_mqtt_publish_state();
}

static void app_mqtt_republish_boost_task(void *arg)
{
    (void)arg;
    vTaskDelay(pdMS_TO_TICKS(3000));
    if (app_mqtt_connected_safe()) {
        ESP_LOGI(TAG, "3s boost: re-publish attributes + state to ensure liveness");
        app_mqtt_publish_attributes();
        app_mqtt_publish_state();
    }
    vTaskDelete(NULL);
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
        if (xTaskCreate(app_mqtt_republish_boost_task, "mqtt_boost", 3072, NULL, 4, NULL) != pdPASS) {
            ESP_LOGW(TAG, "Failed to create mqtt_boost task");
        }
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

void app_mqtt_stop(void)
{
    esp_mqtt_client_handle_t client_to_destroy = NULL;

    app_mqtt_ensure_runtime_init();

    app_mqtt_lock();
    if (s_mqtt.telemetry_task != NULL) {
        xEventGroupSetBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY);
    }
    client_to_destroy = s_mqtt.client;
    s_mqtt.client = NULL;
    s_mqtt.connected = false;
    app_mqtt_unlock();

    if (s_mqtt.telemetry_task != NULL) {
        EventBits_t bits;
        bits = xEventGroupWaitBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY,
                                   pdFALSE, pdTRUE, pdMS_TO_TICKS(12000));
        if ((bits & MQTT_STOP_EVENT_TELEMETRY) == 0) {
            ESP_LOGW(TAG, "Telemetry task did not exit within timeout, forcing NULL handle");
        }
        app_mqtt_lock();
        s_mqtt.telemetry_task = NULL;
        app_mqtt_unlock();
    }

    if (client_to_destroy != NULL) {
        esp_mqtt_client_stop(client_to_destroy);
        esp_mqtt_client_destroy(client_to_destroy);
    }
}

esp_err_t app_mqtt_apply_config(void)
{
    esp_mqtt_client_config_t mqtt_cfg = {0};
    const char *will_payload = "{\"online\":false}";
    esp_mqtt_client_handle_t new_client = NULL;
    bool is_tb_backend;
    esp_err_t err;

    app_mqtt_ensure_runtime_init();
    app_mqtt_stop();

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
    return ESP_OK;
}

void app_mqtt_restart_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(200));
    if (app_mqtt_apply_config() != ESP_OK) {
        ESP_LOGE(TAG, "MQTT apply config failed");
    }
    vTaskDelete(NULL);
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

esp_err_t app_mqtt_start(void)
{
    esp_err_t err;
    BaseType_t rt;
    TaskHandle_t existing = NULL;

    app_mqtt_ensure_runtime_init();

    app_mqtt_lock();
    existing = s_mqtt.telemetry_task;
    s_mqtt.started = true;
    app_mqtt_unlock();

    if (existing != NULL) {
        ESP_LOGW(TAG, "Telemetry task already running, skipping create");
    } else {
        xEventGroupClearBits(s_mqtt.stop_events, MQTT_STOP_EVENT_TELEMETRY);
        rt = xTaskCreate(app_mqtt_telemetry_task, "mqtt_telemetry", 4096, NULL, 5, &existing);
        if (rt != pdPASS) {
            app_copy_string(s_mqtt.last_error, sizeof(s_mqtt.last_error), "telemetry_task_failed");
            err = app_mqtt_apply_config();
            return (err == ESP_OK) ? ESP_FAIL : err;
        }
        app_mqtt_lock();
        s_mqtt.telemetry_task = existing;
        app_mqtt_unlock();
    }

    err = app_mqtt_apply_config();
    return err;
}
