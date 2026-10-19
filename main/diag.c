#include <stdbool.h>
#include <stdio.h>
#include <stdint.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/task.h"

#include "bt.h"
#include "bt_gattc.h"
#include "bt_gatts.h"
#include "constants.h"
#include "data_pipe.h"
#include "diag.h"
#include "http_priv.h"
#include "mem_guard.h"
#include "mqtt.h"

static const char *TAG = "diag";

/* 1.3-44:基准节拍 1s(堆低谷采样);例行 dump 30s;栈高水位 10 分钟 */
#define APP_DIAG_TICK_MS 1000
#define APP_DIAG_TASK_STACK 3072
#define APP_DIAG_TASK_PRIO 2

/* 无效值:接口未启用/查询失败时使用,避免把 0 误读成"信道 0" */
#define APP_DIAG_UNKNOWN_CH 0xFFu

/* 只读快照。任一子查询失败只影响该字段,绝不阻塞、不重试。
 * 注意:APSTA 下 AP 与 STA **共享同一个信道**(IDF 的 esp_wifi_get_channel 无接口
 * 参数就是证据),因此这个信道一旦因 STA 重连而变化,已连上 AP 的客户端必须跟着
 * 重新关联 —— 这正是"手机连着 AP 却打不开页面"的头号可疑机制,故必须逐周期可见。 */
static void app_diag_dump(void)
{
    const size_t heap_free = esp_get_free_heap_size();
    const size_t heap_min = esp_get_minimum_free_heap_size();
    const size_t heap_largest = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    data_pipe_stats_t dp_stats;
    wifi_mode_t mode = WIFI_MODE_NULL;
    uint8_t primary = APP_DIAG_UNKNOWN_CH;
    wifi_second_chan_t second = WIFI_SECOND_CHAN_NONE;
    bool channel_valid = false;
    int ap_clients = -1;

    data_pipe_get_stats(&dp_stats);

    if (esp_wifi_get_mode(&mode) == ESP_OK) {
        /* 签名:esp_wifi_get_channel(uint8_t *primary, wifi_second_chan_t *second) */
        channel_valid = (esp_wifi_get_channel(&primary, &second) == ESP_OK);
        if ((mode & WIFI_MODE_AP) != 0) {
            wifi_sta_list_t sta_list;
            if (esp_wifi_ap_get_sta_list(&sta_list) == ESP_OK) {
                ap_clients = (int)sta_list.num;
            }
        }
    }

    /* 1.3-44 全开稳态观测:level 是全局内存水位档(见 mem_guard.h);
     * mqtt_shed/pipe_yield 是让路计数 —— "极端情况下系统选择了降级而不是崩溃"
     * 的直接证据(0 表示从未触发,非 0 表示触发过且已恢复)。 */
    ESP_LOGI(TAG, "heap free=%u min=%u largest=%u level=%s | wifi mode=%d ch=%u/%d ap_clients=%d"
             " mqtt_outbox=%d mqtt_shed=%u pipe_yield=%u",
             (unsigned)heap_free,
             (unsigned)heap_min,
             (unsigned)heap_largest,
             app_mem_level_str(app_mem_level()),
             (int)mode,
             channel_valid ? (unsigned)primary : APP_DIAG_UNKNOWN_CH,
             channel_valid ? (int)second : -1,
             ap_clients,
             app_mqtt_outbox_size(),
             (unsigned)app_mqtt_tele_shed_count(),
             (unsigned)dp_stats.yielded);
}

/* 1.3-30 批 1 核绑定可见性:启动时打印 wifi/tcpip/httpd 三个任务的落核。
 * xTaskGetCoreID 对未钉核任务返回 tskNO_AFFINITY(-1),对钉核任务返回核号。
 * 预期(本批 A+B+C):wifi=1 tcpip=1 httpd=1;BT 控制器/主机按官方要求仍钉 core 0。 */
static void app_diag_dump_core_affinity(void)
{
    static const char *names[] = {"wifi", "tcpip", "httpd"};
    char line[96];
    size_t used = 0;
    int n;
    size_t i;

    for (i = 0; i < sizeof(names) / sizeof(names[0]); i++) {
        TaskHandle_t h = xTaskGetHandle(names[i]);
        BaseType_t core = (h != NULL) ? xTaskGetCoreID(h) : -2; /* -2 = 任务不存在 */
        n = snprintf(line + used, sizeof(line) - used, "%s%s=%ld",
                     (i == 0) ? "" : " ", names[i], (long)core);
        if (n < 0 || (size_t)n >= sizeof(line) - used) {
            break;
        }
        used += (size_t)n;
    }
    ESP_LOGI(TAG, "core_affinity: %s", line);
}

/* ---------------------------------------------------------------------------
 * 1.3-30 核分载量化观测:FreeRTOS 运行时间统计(ESP_TIMER 1MHz)。
 * 每 30s 打**每核空闲率**:用 idle 任务运行计数器的**增量**计算 —— 32 位计数器
 * 约 71 分钟回绕,增量相减天然回绕安全(间隔远小于回绕周期);
 * 每 10 分钟(20 周期)再打一次全任务占比表,看各任务落在哪、占多少。
 * ------------------------------------------------------------------------- */
static uint32_t s_idle0_prev;
static uint32_t s_idle1_prev;
static int64_t s_idle_prev_us;
static bool s_idle_first = true;
static unsigned s_diag_cycle = 0;

static void app_diag_dump_core_idle(void)
{
#if configGENERATE_RUN_TIME_STATS
    uint32_t c0 = ulTaskGetIdleRunTimeCounterForCore(0);
    uint32_t c1 = ulTaskGetIdleRunTimeCounterForCore(1);
    int64_t now_us = esp_timer_get_time();

    if (!s_idle_first) {
        uint32_t d0 = c0 - s_idle0_prev;    /* uint32 减法:回绕安全 */
        uint32_t d1 = c1 - s_idle1_prev;
        int64_t interval_us = now_us - s_idle_prev_us;

        if (interval_us > 0) {
            unsigned pct0 = (unsigned)((uint64_t)d0 * 100u / (uint64_t)interval_us);
            unsigned pct1 = (unsigned)((uint64_t)d1 * 100u / (uint64_t)interval_us);
            ESP_LOGI(TAG, "core idle: core0=%u%% core1=%u%%", pct0, pct1);
        }
    }
    s_idle_first = false;
    s_idle0_prev = c0;
    s_idle1_prev = c1;
    s_idle_prev_us = now_us;

    s_diag_cycle++;
    if (s_diag_cycle % 20 == 0) {
        static char stats_buf[2048];

        vTaskGetRunTimeStats(stats_buf);
        ESP_LOGI(TAG, "task runtime stats:\n%s", stats_buf);
    }
#else
    (void)s_idle0_prev; (void)s_idle1_prev; (void)s_idle_prev_us;
#endif
}

/* 存储自检(诊断用,1.3-36 后续):开机读一次 config.html.gz,
 * 区分"littlefs 读坏"与"httpd 上下文问题"。
 * 1.3-89:页面发送或内存非 normal 时不打开文件 —— 自检本身要占 FILE/FD,
 * 在低堆期会和页面发送抢最后一个可分配块;跳过只损失一次诊断,不影响业务。 */
static void app_diag_storage_selftest(void)
{
    static uint32_t s_skip_count;
    app_mem_level_t level = app_mem_level();
    FILE *f;
    char buf[64];
    size_t got;

    if (app_http_serving_page() || level != APP_MEM_NORMAL) {
        s_skip_count++;
        if (s_skip_count <= 3u || (s_skip_count % 20u) == 0u) {
            ESP_LOGW(TAG,
                     "storage selftest skipped #%u: page=%d level=%s free=%u largest=%u",
                     (unsigned)s_skip_count, app_http_serving_page() ? 1 : 0,
                     app_mem_level_str(level),
                     (unsigned)esp_get_free_heap_size(),
                     (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT));
        }
        return;
    }
    f = fopen(APP_BASE_PATH "/config.html.gz", "rb");
    if (f == NULL) {
        ESP_LOGE(TAG, "storage selftest: fopen failed");
        return;
    }
    got = fread(buf, 1, sizeof(buf), f);
    ESP_LOGI(TAG, "storage selftest: first_read=%u ferror=%d feof=%d",
             (unsigned)got, ferror(f), feof(f));
    fclose(f);
}

/* ===========================================================================
 * 1.3-44 全开稳态的"事后现场"两件套(不需要人盯串口)
 * ------------------------------------------------------------------------- */

/* ---- 堆低谷上下文记录(1s 采样) ----
 * esp_get_minimum_free_heap_size() 只给数值,不给上下文。这里每次出现新的
 * 全局最低点就打印一行(附 BLE/MQTT/页面流/出站队列状态)—— 极端情况
 * "最低降到多少、当时什么在跑"可以直接从串口回看。 */
typedef struct {
    size_t free_min;
    size_t largest_at_min;
    uint32_t uptime_s;
    int outbox;
    bool ble_adv;
    bool ble_conn;
    bool mqtt_conn;
    bool page;
} diag_heap_low_t;

static diag_heap_low_t s_heap_low;

/* 只在进入"感兴趣区间"(<32KB)后才开始记录 —— 开机阶段从 100KB+ 一路下探到
 * 稳态 16KB 的新低是每次上电的常态,没有记录价值,只会淹没有意义的日志。 */
#define APP_DIAG_HEAP_LOW_LOG_LIMIT 32768

static void app_diag_sample_heap(void)
{
    size_t free_now = esp_get_free_heap_size();

    if (free_now >= APP_DIAG_HEAP_LOW_LOG_LIMIT) {
        return;
    }
    if (s_heap_low.free_min != 0 && free_now >= s_heap_low.free_min) {
        return;
    }
    s_heap_low.free_min = free_now;
    s_heap_low.largest_at_min = heap_caps_get_largest_free_block(MALLOC_CAP_DEFAULT);
    s_heap_low.uptime_s = (uint32_t)(esp_timer_get_time() / 1000000);
    s_heap_low.ble_adv = app_bt_advertising();
    s_heap_low.ble_conn = app_ble_gatts_connected() || app_ble_gattc_connected();
    app_mqtt_lock();
    s_heap_low.mqtt_conn = s_mqtt.connected;
    app_mqtt_unlock();
    s_heap_low.outbox = app_mqtt_outbox_size();
    s_heap_low.page = app_http_serving_page();
    ESP_LOGW(TAG, "heap new low: free=%u largest=%u t=%us ble_adv=%d ble_conn=%d mqtt=%d page=%d outbox=%d",
             (unsigned)s_heap_low.free_min,
             (unsigned)s_heap_low.largest_at_min,
             (unsigned)s_heap_low.uptime_s,
             (int)s_heap_low.ble_adv, (int)s_heap_low.ble_conn,
             (int)s_heap_low.mqtt_conn, (int)s_heap_low.page, s_heap_low.outbox);
}

/* ---- 任务栈高水位(10 分钟一次/开机 60s 一次) ----
 * 逐个按名字查句柄再查历史最低剩余 —— **零 DRAM 增量**:名字表是 const(放
 * flash),不用 TaskStatus_t 快照数组(那只为 10 分钟一次的报表就要吃掉
 * ~1.3KB BSS,在 16.7KB 预算下不划算)。uxTaskGetStackHighWaterMark 的单位
 * 是 StackType_t 个字(IDF prvTaskCheckFreeStackSpace 按 字节/sizeof(StackType_t)
 * 折算),×4 还原为字节。名字查不到的任务自动跳过(模式未启用时不产生噪音)。
 * 本机栈溢出是 canary panic、没有提前预警 —— 这份报告让"哪个任务的栈贴着边跑"
 * 在崩之前可见(历史余量 <1KB 的全部列出 + 全局最小者)。
 *
 * ⚠ 硬约束:xTaskGetHandle 断言 strlen(name) < configMAX_TASK_NAME_LEN(16),
 * 超长名字会直接 assert 崩溃(上机实测:首次调用即 panic 重启,1.3-44 首版 bug)。
 * 因此:① 这里一律用 ≤15 字符;② 任务名被 FreeRTOS 截断到 15 字符存储,查名
 * 必须用**截断后的形态**(如 "wifi_auto_connect" → "wifi_auto_conne");
 * ③ 循环里有 strlen 兜底,以后即使加错长名也只跳过、不崩。 */
static const char *const s_stack_watch[] = {
    /* BT 栈(全开稳态的头号嫌疑;名字经 libbt.a 符号实查) */
    "btController", "BTU_TASK", "BTC_TASK", "hciT",
    /* 网络与服务 */
    "wifi", "tcpip", "httpd", "http_reaper", "captive_dns",
    /* 应用任务(15 字符上限;过长者用被 FreeRTOS 截断后的名字) */
    "mqtt_task", "mqtt_telemetry", "mqtt_boost", "mqtt_restart",
    "data_pipe", "ble_cmd", "uart_console", "app_diag",
    "wifi_auto_conne", "wifi_restart", "wifi_boot_monit",
    "ip_conflict", "ota_confirm", "factory_reset_b", "main",
    /* IDF 常驻 */
    "esp_timer", "sys_evt", "Tmr Svc", "IDLE0", "IDLE1",
};

static char s_stack_line[384];

static void app_diag_dump_stack_water(void)
{
    size_t min_bytes = (size_t)-1;
    const char *min_name = "?";
    size_t used = 0;
    size_t i;

    s_stack_line[0] = '\0';
    for (i = 0; i < sizeof(s_stack_watch) / sizeof(s_stack_watch[0]); i++) {
        TaskHandle_t h;
        size_t bytes;
        int n;

        if (strlen(s_stack_watch[i]) >= 16) {   /* configMAX_TASK_NAME_LEN:xTaskGetHandle 会断言 */
            ESP_LOGW(TAG, "stack watch name too long, skipped: %s", s_stack_watch[i]);
            continue;
        }
        h = xTaskGetHandle(s_stack_watch[i]);
        if (h == NULL) {
            continue;   /* 任务不存在(该模式未启用/未创建) */
        }
        bytes = (size_t)uxTaskGetStackHighWaterMark(h) * sizeof(StackType_t);
        if (bytes < min_bytes) {
            min_bytes = bytes;
            min_name = s_stack_watch[i];
        }
        if (bytes >= 2048) {
            continue;   /* 只列"贴线"的(历史余量 <2KB;1.3-48 从 1KB 放宽,给 MQTT 任务取证) */
        }
        n = snprintf(s_stack_line + used, sizeof(s_stack_line) - used, "%s%s=%uB",
                     (used > 0) ? " " : "", s_stack_watch[i], (unsigned)bytes);
        if (n < 0 || (size_t)n >= sizeof(s_stack_line) - used) {
            break;
        }
        used += (size_t)n;
    }
    if (min_bytes == (size_t)-1) {
        ESP_LOGW(TAG, "stack water: no watch tasks found");
        return;
    }
    ESP_LOGI(TAG, "stack water: min=%s:%uB%s%s",
             min_name, (unsigned)min_bytes,
             (used > 0) ? " low:" : "", s_stack_line);
}

static void app_diag_task(void *arg)
{
    uint32_t sec = 0;

    (void)arg;

    app_diag_dump_core_affinity();
    app_diag_storage_selftest();
    app_diag_dump();
    for (;;) {
        /* 1.3-44:基准节拍 1s(堆低谷采样);每 30s 例行 dump/空闲率;
         * 10 分钟(开机 60s 加一次)打任务栈高水位。 */
        vTaskDelay(pdMS_TO_TICKS(APP_DIAG_TICK_MS));
        sec++;
        app_diag_sample_heap();
        if ((sec % 30u) == 0u) {
            app_diag_storage_selftest();
            app_diag_dump_core_idle();
            app_diag_dump();
        }
        if ((sec % 600u) == 0u || sec == 60u) {
            app_diag_dump_stack_water();
        }
    }
}

esp_err_t app_diag_start(void)
{
    if (xTaskCreate(app_diag_task, "app_diag", APP_DIAG_TASK_STACK, NULL,
                    APP_DIAG_TASK_PRIO, NULL) != pdPASS) {
        ESP_LOGE(TAG, "task create failed");
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
