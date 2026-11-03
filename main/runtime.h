#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "constants.h"

/* ===== M1 启动安全:配置应用阶段/失败记录/网络配置状态(设计稿 静态IP_M1启动安全设计稿_2026-11-03.md §3.1) ===== */

/* 配置应用阶段:失败时记录"首个失败阶段"(根因,不被后续恢复覆盖) */
typedef enum {
    APP_WIFI_APPLY_STAGE_NONE = 0,
    APP_WIFI_APPLY_STAGE_WIFI_STOP,
    APP_WIFI_APPLY_STAGE_SET_MODE,
    APP_WIFI_APPLY_STAGE_SET_CONFIG,
    APP_WIFI_APPLY_STAGE_STATIC_DHCPC_STOP,
    APP_WIFI_APPLY_STAGE_STATIC_PARSE,
    APP_WIFI_APPLY_STAGE_STATIC_SET_IP,
    APP_WIFI_APPLY_STAGE_STATIC_SET_DNS,   /* 预留:M2 检查 DNS 返回值后启用 */
    APP_WIFI_APPLY_STAGE_DHCPC_START,
    APP_WIFI_APPLY_STAGE_WIFI_START,
} app_wifi_apply_stage_t;

/* 网络配置运行状态(M5 才对外暴露;M1 仅 RAM+日志) */
typedef enum {
    APP_NET_CFG_STATE_DHCP = 0,
    APP_NET_CFG_STATE_STATIC,
    APP_NET_CFG_STATE_FALLBACK,                /* 静态失败后 DHCP 救援运行中 */
    APP_NET_CFG_STATE_OFFLINE,                 /* 离线待配置,Wi-Fi 已停止 */
    APP_NET_CFG_STATE_OFFLINE_STOP_FAILED,     /* 离线,但 esp_wifi_stop() 实际执行并失败 */
    APP_NET_CFG_STATE_OFFLINE_QUIESCE_TIMEOUT, /* 离线,但连接调用未退出,未执行 stop */
    /* M4: APP_NET_CFG_STATE_PENDING / ROLLBACK_STATIC */
} app_net_cfg_state_t;

/* 一次可应用的 STA 配置快照(同时作为 RAM LKG 的承载结构) */
typedef struct {
    bool valid;
    bool sta_use_static;
    char ssid[33];
    char password[65];
    char static_ip[16];
    char static_gw[16];
    char static_mask[16];
    char static_dns1[16];
    char static_dns2[16];
} app_wifi_apply_cfg_t;

/* 配置应用失败记录(本启动周期首个失败;stage/err 不被恢复动作覆盖) */
typedef struct {
    bool valid;
    app_wifi_apply_stage_t stage;
    esp_err_t err;
    bool static_mode;
    uint8_t attempts;            /* 应用尝试次数(候选/LKG/救援各计 1) */
    char recovery[16];           /* none | lkg_restore | dhcp_rescue | offline */
    esp_err_t offline_stop_err;  /* esp_wifi_stop() 失败原始错误;ESP_OK=停止成功 */
} app_wifi_apply_failure_t;

typedef struct {
    SemaphoreHandle_t lock;
    EventGroupHandle_t stop_events;
    void *telemetry_task;
    void *boost_task;
    void *restart_task;
    void *client;
    bool connected;
    bool started;
    bool fatal_error;  /* 认证被拒等致命错误:停止自动重连,等配置变更后重试(1.3-43) */
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
    bool sta_ip_conflict;       /* 静态 IP 冲突检测结果(ARP 探测,见 wifi.c) */
    uint8_t sta_retries;
    int64_t startup_connect_deadline_ms;
    uint8_t scan_records_storage[WIFI_SCAN_LIST_SIZE * 512];
    uint16_t scan_count;
    char sta_ip[16];
    char sta_gateway[16];
    char sta_netmask[16];
    char last_disconnect[32];
    /* 运行态：实际关联上的 SSID / 对应的已保存 profile 下标。
     * 与 s_config.sta_ssid（正式配置）区分开：自动回退时二者不同，
     * App 只有靠这两个字段才能判断"当前连的到底是哪个网络"。 */
    char connected_ssid[33];
    int connected_profile;
    char startup_fallback_reason[32];
    void *restart_task;
    /* ===== M1 启动安全(§3.1/§3.6):以下字段均受 runtime lock 保护 ===== */
    app_net_cfg_state_t cfg_state;          /* 网络配置运行状态(唯一状态源) */
    app_wifi_apply_failure_t apply_failure; /* 本启动周期应用失败记录 */
    app_wifi_apply_cfg_t lkg;               /* 最近一次"成功应用"的配置快照 */
    bool cfg_offline;                       /* 离线门控:封锁一切自动连接(显式 tryconnect 在入口拒绝) */
    uint32_t connect_inflight;              /* 已取得连接许可、尚未返回的调用数(仅 begin/end 可改写) */
} wifi_runtime_t;
