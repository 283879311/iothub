#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "constants.h"

typedef struct {
    SemaphoreHandle_t lock;
    EventGroupHandle_t stop_events;
    void *telemetry_task;
    void *client;
    bool connected;
    bool started;
    uint32_t publish_count;
    int last_msg_id;
    char last_error[96];
    char uri[128];
    char client_id[48];
} mqtt_runtime_t;

typedef struct {
    bool sta_connected;
    bool sta_has_ip;
    bool startup_connect_active;
    bool startup_fallback_to_ap;
    uint8_t sta_retries;
    int64_t startup_connect_deadline_ms;
    uint8_t scan_records_storage[WIFI_SCAN_LIST_SIZE * 512];
    uint16_t scan_count;
    char sta_ip[16];
    char sta_gateway[16];
    char sta_netmask[16];
    char last_disconnect[32];
    char startup_fallback_reason[32];
} wifi_runtime_t;
