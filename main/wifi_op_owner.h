#pragma once

#include <stdbool.h>

/* WIFI-OP-RESOURCE-RACE(2026-11-12) 的纯逻辑部分：STA 操作所有权状态机。
 * 与 operation_state.* 同一先例——不依赖 ESP-IDF/FreeRTOS，主机可测。
 *
 * 设计口径见 docs/WIFI-OP-RESOURCE-RACE-修订设计_B1-B5_2026-11-12.md：
 *  - 六值 owner；同一时刻只有一个主体；
 *  - CLEANUP 由 cleanup 请求直接取得，或由任一持有者在释放时移交（无 NONE 窗口）；
 *  - TRYX 等待者优先（waiter 置位期间 AUTO 一律被拒）；
 *  - CLEANUP_FAILED 粘性：拒绝一切取得，仅 BOOT_RESET 清除。 */

typedef enum {
    APP_WIFI_OWNER_NONE = 0,
    APP_WIFI_OWNER_AUTO,           /* 后台自动连接 */
    APP_WIFI_OWNER_TRYX,           /* tryconnect 生命周期 */
    APP_WIFI_OWNER_APPLY,          /* boot / 显式配置应用 */
    APP_WIFI_OWNER_CLEANUP,        /* cancel/expire 恢复链（含 apply_cfg 子阶段） */
    APP_WIFI_OWNER_CLEANUP_FAILED, /* 粘性失败：禁止一切 STA 发起 */
    APP_WIFI_OWNER_COUNT,
} app_wifi_owner_t;

typedef enum {
    /* TRYX 一次取得尝试（等待循环里反复投递；waiter 位由调用方在首轮前置位） */
    APP_WIFI_OWNER_EV_CLAIM_TRYX = 0,
    APP_WIFI_OWNER_EV_TRYX_GIVEUP,     /* 超时/取消：清 waiter */
    APP_WIFI_OWNER_EV_CLAIM_AUTO,
    APP_WIFI_OWNER_EV_CLAIM_APPLY,
    APP_WIFI_OWNER_EV_RELEASE_TRYX,
    APP_WIFI_OWNER_EV_RELEASE_AUTO,
    APP_WIFI_OWNER_EV_RELEASE_APPLY,
    APP_WIFI_OWNER_EV_CLEANUP_REQUEST,
    APP_WIFI_OWNER_EV_CLEANUP_DONE,
    APP_WIFI_OWNER_EV_CLEANUP_FAIL,
    APP_WIFI_OWNER_EV_BOOT_RESET,
} app_wifi_owner_ev_t;

typedef enum {
    APP_WIFI_OWNER_ACT_NONE = 0,
    APP_WIFI_OWNER_ACT_ACCEPT,       /* 已取得 */
    APP_WIFI_OWNER_ACT_WAIT,         /* 忙，继续等（TRYX/APPLY 阻塞取得） */
    APP_WIFI_OWNER_ACT_DENY_BUSY,    /* 非阻塞取得被拒（owner 被占） */
    APP_WIFI_OWNER_ACT_DENY_WAITER,  /* AUTO 被 TRYX 等待者拒绝 */
    APP_WIFI_OWNER_ACT_DENY_FAILED,  /* CLEANUP_FAILED 粘性拒绝 */
    APP_WIFI_OWNER_ACT_NOTIFY,       /* cleanup 请求成功，需通知 restart 任务 */
    APP_WIFI_OWNER_ACT_HANDOVER,     /* 释放时移交 CLEANUP，需通知 restart 任务 */
} app_wifi_owner_act_t;

typedef struct {
    app_wifi_owner_t owner;
    bool tryx_waiting;
    bool cleanup_requested;
} app_wifi_owner_state_t;

typedef struct {
    app_wifi_owner_state_t state;
    app_wifi_owner_act_t action;
} app_wifi_owner_step_t;

/* 初始状态（boot） */
app_wifi_owner_state_t app_wifi_owner_init(void);

/* 纯迁移：输入当前状态与事件，返回新状态 + 动作（不产生副作用）。 */
app_wifi_owner_step_t app_wifi_owner_step(const app_wifi_owner_state_t *in,
                                          app_wifi_owner_ev_t ev);

/* 名字（日志/测试用） */
const char *app_wifi_owner_name(app_wifi_owner_t owner);
const char *app_wifi_owner_act_name(app_wifi_owner_act_t act);
const char *app_wifi_owner_ev_name(app_wifi_owner_ev_t ev);

/* 取得前的快速判定（供 schedule 入口等非阻塞点使用同一口径）：
 * true = 该事件此刻会被接受。 */
bool app_wifi_owner_would_accept(const app_wifi_owner_state_t *st,
                                 app_wifi_owner_ev_t ev);
