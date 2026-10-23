#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_random.h"
#include "esp_timer.h"
#include "nvs.h"

#include "common.h"
#include "config.h"
#include "data_pipe.h"
#include "wifi.h"

#define APP_NAMESPACE "iothub"
#define APP_CONFIG_KEY "runtime_cfg"
#define APP_WIFI_PROFILES_KEY "wifi_profiles"
#define APP_CONFIG_MAGIC 0x494f5448UL
#define APP_CONFIG_VERSION 11
#define APP_IO_RELAY_LAST_KEY "io_relay_last"
#define APP_IO_LED_LAST_KEY   "io_led_last"
#define APP_WIFI_PROFILES_MAGIC 0x57494649UL
#define APP_WIFI_PROFILES_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX];
} app_wifi_profiles_blob_t;

/* G2 事务快照：只允许一个活动事务，固定内存，不动态分配。 */
typedef struct {
    bool valid;
    app_config_t config;
    app_wifi_profiles_blob_t profiles;
} app_config_snapshot_blob_t;

static app_config_snapshot_blob_t s_config_snapshot;

/*
 * Legacy config structs — field layout by version:
 *
 *   v3 (struct layout size = sizeof(app_config_v3_t)):
 *     + contains legacy uart_rx_gpio / uart_tx_gpio (int32)
 *     + no GPIO pin fields for relay/led/input (pins were fixed-constant in v3 era)
 *     + all configs below have the common prefix (relay_active_high..uart_baudrate)
 *       and the legacy AP/STA/MQTT string block. AP slots are NVS-layout-only now;
 *       current runtime uses sta_ssid..mqtt_token (see migrate helpers below).
 *
 *   v4 (sizeof(app_config_v4_t)):
 *     + keeps uart_rx_gpio / uart_tx_gpio from v3
 *     + ADDS three more legacy GPIO fields: relay_gpio, led_gpio, input_gpio (int32)
 *       — these were later removed because GPIO pinout became a compile-time constant
 *       handled by app_relay_gpio()/app_led_gpio()/app_input_gpio() (see state.c)
 *
 *   v5 (sizeof(app_config_v5_t)):
 *     + REMOVES all five legacy GPIO fields (uart_rx/tx_gpio AND relay/led/input_gpio)
 *       because every pin was validated against a compile-time fixed layout via
 *       app_validate_fixed_gpio_config() (later removed entirely to eliminate dead code)
 *     + matches field order of the current v6 except for the two uart frame bytes
 *       added in v6: uart_data_bits and uart_stop_bits — restored by the
 *       APP_MIGRATE_FIXED_FRAME() macro during migration
 *
 *   v6 (sizeof(app_config_v6_t)):
 *     + no legacy GPIO pin fields at all (pins are compile-time fixed)
 *     + adds uart_data_bits (uint8) and uart_stop_bits (uint8) so the UART frame is
 *       fully described inside config without the hard-coded 8N1 assumption
 *
 *   v7 (APP_CONFIG_VERSION = 7):
 *     + appends web 登录凭据(web_username/web_pass_salt/web_pass_hash)到结构体末尾;
 *       全部为 1 字节对齐成员,因此 v6 blob 是 v7 的字节前缀——迁移时读入前缀、
 *       保留 set_defaults 生成的默认凭据即可
 *
 *   v8 (APP_CONFIG_VERSION = 8):
 *     + appends bt_role(蓝牙角色:0=off/1=peripheral/2=central)到结构体末尾;
 *       v7 blob 同样是 v8 的字节前缀——迁移时读入前缀,再由 v7 的 bt_mode 推导:
 *       bt_mode==1(ble) → peripheral,其余 → off
 *
 *   v9 (APP_CONFIG_VERSION = 9 — app_config_t):
 *     + appends STA 静态 IP 字段(sta_use_static / sta_static_ip / gateway / netmask / dns1 / dns2);
 *       v8 blob 是 v9 的字节前缀——迁移时读入前缀,新字段由 set_defaults 初始化为 DHCP。
 *
 *   v11 (current APP_CONFIG_VERSION = 11 — app_config_t):
 *     + appends relay_power_policy / led_power_policy(0=固定关/1=固定开/2=状态记忆);
 *       v10 blob 是 v11 的字节前缀——迁移时读入前缀,新字段由 set_defaults 初始化为固定关。
 *       输出上次状态不再写主 blob,而是独立 NVS 键(仅状态记忆策略写)。
 */

/* Legacy v3-v6 mirrors: the AP slots below are kept only to read old blob
 * layouts; app_config_migrate_* never copies them into the current config. */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t bt_mode;
    uint8_t net_mode;
    uint8_t mqtt_use_tls;
    uint8_t uart_parity_mode;
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_v5_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t bt_mode;
    uint8_t net_mode;
    uint8_t mqtt_use_tls;
    uint8_t uart_parity_mode;
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    int32_t uart_rx_gpio;
    int32_t uart_tx_gpio;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_v3_t;

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    int32_t relay_gpio;          /* v4-only: legacy compile-relay pin, now fixed-constant */
    int32_t led_gpio;            /* v4-only: legacy compile-led pin, now fixed-constant */
    int32_t input_gpio;          /* v4-only: legacy compile-input pin, now fixed-constant */
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t bt_mode;
    uint8_t net_mode;
    uint8_t mqtt_use_tls;
    uint8_t uart_parity_mode;
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    int32_t uart_rx_gpio;
    int32_t uart_tx_gpio;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_v4_t;

/* v6 镜像:与 v7(app_config_t)的前缀字节完全一致,仅无 web 凭据字段 */
typedef struct {
    uint32_t magic;
    uint16_t version;
    uint8_t relay_active_high;
    uint8_t led_active_high;
    uint8_t input_active_low;
    uint8_t relay_on;
    uint8_t led_on;
    uint8_t bt_mode;
    uint8_t net_mode;
    uint8_t mqtt_use_tls;
    uint8_t uart_parity_mode;
    uint8_t uart_data_bits;
    uint8_t uart_stop_bits;
    uint16_t mqtt_port;
    uint32_t uart_baudrate;
    char ap_ssid[33];
    char ap_password[65];
    char sta_ssid[33];
    char sta_password[65];
    char bt_device_name[33];
    char mqtt_backend[16];
    char mqtt_host[64];
    char mqtt_token[128];
} app_config_v6_t;

static const char *TAG = "iothub";

extern app_config_t s_config;

static SemaphoreHandle_t s_config_lock = NULL;
static SemaphoreHandle_t s_wifi_runtime_lock = NULL;
static SemaphoreHandle_t s_wifi_profiles_lock = NULL;
/* 静态创建所需缓冲与创建保护:消除首次并发调用双创建的竞态 */
static StaticSemaphore_t s_config_lock_buf;
static StaticSemaphore_t s_wifi_runtime_lock_buf;
static StaticSemaphore_t s_wifi_profiles_lock_buf;
static portMUX_TYPE s_config_locks_mux = portMUX_INITIALIZER_UNLOCKED;

void app_config_lock(void)
{
    SemaphoreHandle_t lock = app_mutex_ensure(&s_config_lock, &s_config_lock_buf,
                                              &s_config_locks_mux, true);
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void app_config_unlock(void)
{
    if (s_config_lock != NULL) {
        xSemaphoreGiveRecursive(s_config_lock);
    }
}

void app_wifi_runtime_lock(void)
{
    SemaphoreHandle_t lock = app_mutex_ensure(&s_wifi_runtime_lock, &s_wifi_runtime_lock_buf,
                                              &s_config_locks_mux, true);
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void app_wifi_runtime_unlock(void)
{
    if (s_wifi_runtime_lock != NULL) {
        xSemaphoreGiveRecursive(s_wifi_runtime_lock);
    }
}

void app_wifi_profiles_lock(void)
{
    SemaphoreHandle_t lock = app_mutex_ensure(&s_wifi_profiles_lock, &s_wifi_profiles_lock_buf,
                                              &s_config_locks_mux, true);
    xSemaphoreTakeRecursive(lock, portMAX_DELAY);
}

void app_wifi_profiles_unlock(void)
{
    if (s_wifi_profiles_lock != NULL) {
        xSemaphoreGiveRecursive(s_wifi_profiles_lock);
    }
}

#define APP_COPY_STR(field_name) \
    app_copy_string(config->field_name, sizeof(config->field_name), legacy->field_name)

#define APP_ASSIGN_UINT8(field_name) \
    config->field_name = legacy->field_name

#define APP_ASSIGN_INT32(field_name) \
    config->field_name = legacy->field_name

#define APP_ASSIGN_UINT16(field_name) \
    config->field_name = legacy->field_name

#define APP_ASSIGN_UINT32(field_name) \
    config->field_name = legacy->field_name

#define APP_MIGRATE_COMMON_PREFIX()                              \
    do {                                                         \
        APP_ASSIGN_UINT8(relay_active_high);                     \
        APP_ASSIGN_UINT8(led_active_high);                       \
        APP_ASSIGN_UINT8(input_active_low);                      \
        APP_ASSIGN_UINT8(relay_on);                              \
        APP_ASSIGN_UINT8(led_on);                                \
        APP_ASSIGN_UINT8(bt_mode);                               \
        APP_ASSIGN_UINT8(net_mode);                              \
        APP_ASSIGN_UINT8(mqtt_use_tls);                          \
        APP_ASSIGN_UINT8(uart_parity_mode);                      \
        APP_ASSIGN_UINT16(mqtt_port);                            \
        APP_ASSIGN_UINT32(uart_baudrate);                        \
    } while (0)

#define APP_MIGRATE_FIXED_FRAME()                                \
    do {                                                         \
        config->uart_data_bits = 8;                              \
        config->uart_stop_bits = 1;                              \
    } while (0)

static void app_config_init_wifi_profiles(app_wifi_profiles_blob_t *blob)
{
    if (blob == NULL) {
        return;
    }

    memset(blob, 0, sizeof(*blob));
    blob->magic = APP_WIFI_PROFILES_MAGIC;
    blob->version = APP_WIFI_PROFILES_VERSION;
}

static bool app_config_wifi_profiles_blob_valid(const app_wifi_profiles_blob_t *blob)
{
    return blob != NULL &&
           blob->magic == APP_WIFI_PROFILES_MAGIC &&
           blob->version == APP_WIFI_PROFILES_VERSION &&
           blob->count <= APP_WIFI_PROFILE_MAX;
}

static void app_config_normalize_wifi_profiles(app_wifi_profiles_blob_t *blob)
{
    size_t write_index = 0;
    size_t read_index;

    if (blob == NULL) {
        return;
    }

    for (read_index = 0; read_index < APP_WIFI_PROFILE_MAX; read_index++) {
        if (blob->profiles[read_index].ssid[0] == '\0') {
            continue;
        }
        if (write_index != read_index) {
            blob->profiles[write_index] = blob->profiles[read_index];
        }
        write_index++;
    }
    while (write_index < APP_WIFI_PROFILE_MAX) {
        memset(&blob->profiles[write_index], 0, sizeof(blob->profiles[write_index]));
        write_index++;
    }
    blob->count = 0;
    for (read_index = 0; read_index < APP_WIFI_PROFILE_MAX; read_index++) {
        if (blob->profiles[read_index].ssid[0] != '\0') {
            blob->count++;
        }
    }
}

static esp_err_t app_config_load_wifi_profiles_blob(nvs_handle_t nvs_handle, app_wifi_profiles_blob_t *blob)
{
    size_t size = sizeof(*blob);

    if (blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_init_wifi_profiles(blob);
    if (nvs_get_blob(nvs_handle, APP_WIFI_PROFILES_KEY, blob, &size) != ESP_OK) {
        return ESP_OK;
    }
    if (size != sizeof(*blob) || !app_config_wifi_profiles_blob_valid(blob)) {
        ESP_LOGW(TAG, "Invalid Wi-Fi profiles blob detected, resetting");
        app_config_init_wifi_profiles(blob);
        return ESP_OK;
    }
    app_config_normalize_wifi_profiles(blob);
    return ESP_OK;
}

static esp_err_t app_config_store_wifi_profiles_blob(nvs_handle_t nvs_handle, const app_wifi_profiles_blob_t *blob)
{
    if (blob == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (blob->count == 0) {
        esp_err_t err = nvs_erase_key(nvs_handle, APP_WIFI_PROFILES_KEY);
        if (err == ESP_ERR_NVS_NOT_FOUND) {
            return nvs_commit(nvs_handle);
        }
        if (err != ESP_OK) {
            return err;
        }
        return nvs_commit(nvs_handle);
    }
    ESP_RETURN_ON_FALSE(app_config_wifi_profiles_blob_valid(blob), ESP_ERR_INVALID_STATE, TAG,
                        "invalid wifi profiles blob");
    ESP_RETURN_ON_ERROR(nvs_set_blob(nvs_handle, APP_WIFI_PROFILES_KEY, blob, sizeof(*blob)), TAG,
                        "set wifi profiles blob failed");
    return nvs_commit(nvs_handle);
}

void app_config_set_defaults(app_config_t *config)
{
    if (config == NULL) {
        return;
    }

    memset(config, 0, sizeof(*config));
    config->magic = APP_CONFIG_MAGIC;
    /* NOTE: config->version intentionally NOT assigned here — the single
     * assignment point is app_config_mark_current_version() below. This way a
     * future APP_CONFIG_VERSION bump requires exactly one edit instead of
     * having to sync the defaults value and every migrate call-site. */
    config->relay_active_high = 1;
    config->led_active_high = 1;
    config->input_active_low = 1;
    config->bt_mode = BT_MODE_BLE;
    config->bt_role = BT_ROLE_PERIPHERAL;
    config->net_mode = 0;
    config->mqtt_port = 1883;
    config->uart_parity_mode = UART_PARITY_DISABLE;
    config->uart_data_bits = 8;
    config->uart_stop_bits = 1;
    config->uart_baudrate = 115200;
    config->sta_use_static = 0;
    app_copy_string(config->sta_static_ip, sizeof(config->sta_static_ip), "");
    app_copy_string(config->sta_static_gateway, sizeof(config->sta_static_gateway), "");
    app_copy_string(config->sta_static_netmask, sizeof(config->sta_static_netmask), "");
    app_copy_string(config->sta_static_dns1, sizeof(config->sta_static_dns1), "");
    app_copy_string(config->sta_static_dns2, sizeof(config->sta_static_dns2), "");
    /* 1.3-07 数据面三边路由默认:UART1→BLE、BLE→UART1|MQTT(上行)、MQTT→BLE(下行) */
    config->data_route_uart1 = DATA_PORT_MASK(DATA_PORT_BLE);
    config->data_route_ble = DATA_PORT_MASK(DATA_PORT_UART1) | DATA_PORT_MASK(DATA_PORT_MQTT);
    config->data_route_mqtt = DATA_PORT_MASK(DATA_PORT_BLE);
    /* v11:输出上电策略,默认固定关闭(安全默认);不写上次状态。 */
    config->relay_power_policy = APP_GPIO_POWER_FIXED_OFF;
    config->led_power_policy = APP_GPIO_POWER_FIXED_OFF;
    app_copy_string(config->bt_device_name, sizeof(config->bt_device_name), "iothub-bt");
    app_copy_string(config->mqtt_backend, sizeof(config->mqtt_backend), "tb_cloud");
    app_copy_string(config->mqtt_host, sizeof(config->mqtt_host), "demo.thingsboard.io");
    /* Web 登录凭据:每台设备随机盐 + 出厂默认用户名/口令(可按部署批次经
     * Kconfig 配置)的哈希,明文口令不落在 NVS 或固件编译产物之外 */
    app_copy_string(config->web_username, sizeof(config->web_username), CONFIG_WEB_DEFAULT_USERNAME);
    esp_fill_random(config->web_pass_salt, sizeof(config->web_pass_salt));
    app_web_hash_password(config->web_pass_salt, sizeof(config->web_pass_salt),
                          CONFIG_WEB_DEFAULT_PASSWORD, config->web_pass_hash);
}

bool app_config_scrub_legacy_ap(app_config_t *config)
{
    bool changed = false;
    size_t i;

    if (config == NULL) {
        return false;
    }
    for (i = 0; i < sizeof(config->reserved_ap_ssid); i++) {
        if (config->reserved_ap_ssid[i] != '\0') {
            changed = true;
            break;
        }
    }
    for (i = 0; i < sizeof(config->reserved_ap_password); i++) {
        if (config->reserved_ap_password[i] != '\0') {
            changed = true;
            break;
        }
    }
    if (changed) {
        memset(config->reserved_ap_ssid, 0, sizeof(config->reserved_ap_ssid));
        memset(config->reserved_ap_password, 0, sizeof(config->reserved_ap_password));
    }
    return changed;
}

static inline void app_config_mark_current_version(app_config_t *config)
{
    if (config != NULL) {
        config->version = APP_CONFIG_VERSION;
    }
}

/* 产品基线：BLE 永远是 Peripheral。旧配置的 off/central 在启动时收敛为
 * peripheral；只改 RAM 不够，必须持久化，否则每次启动都会重复迁移。 */
void app_config_force_ble_peripheral(void)
{
    bool changed = false;

    app_config_lock();
    if (s_config.bt_role != BT_ROLE_PERIPHERAL) {
        s_config.bt_role = BT_ROLE_PERIPHERAL;
        changed = true;
    }
    if (s_config.bt_mode != BT_MODE_BLE) {
        s_config.bt_mode = BT_MODE_BLE;
        changed = true;
    }
    if (changed) {
        esp_err_t err = app_config_save(&s_config);

        if (err == ESP_OK) {
            ESP_LOGW(TAG, "BLE role forced to peripheral and persisted");
        } else {
            ESP_LOGE(TAG, "BLE role forced to peripheral, save failed: %s",
                     esp_err_to_name(err));
        }
    }
    app_config_unlock();
}

esp_err_t app_config_save(const app_config_t *config)
{
    static app_config_t s_existing;
    nvs_handle_t nvs_handle;
    esp_err_t err;
    size_t existing_size;
    int64_t save_started_ms;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_config_lock();
    save_started_ms = esp_timer_get_time() / 1000;
    err = nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        app_config_unlock();
        return err;
    }

    existing_size = sizeof(s_existing);
    memset(&s_existing, 0, sizeof(s_existing));
    if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &s_existing, &existing_size) == ESP_OK &&
        existing_size == sizeof(s_existing) &&
        s_existing.magic == APP_CONFIG_MAGIC &&
        s_existing.version == APP_CONFIG_VERSION &&
        memcmp(&s_existing, config, sizeof(s_existing)) == 0) {
        nvs_close(nvs_handle);
        app_config_unlock();
        return ESP_OK;
    }

    err = nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    /* 观测(问题与版本优化.md 1.3-10 ②):NVS 写是在**配置锁内**完成的 —— 这是刻意的,
     * 用于防"读-改-写"丢更新(若解锁后再写,另一写者的新值可能被本次旧快照覆盖)。
     * 锁是递归锁,且只有"确有变化"才会走到这里(上面已与 NVS 现值比对),故真实开销
     * 只发生在**改配置**时。这里把"到底多久"变成可见数字:若频繁 >50ms,再按
     * "快照 + 独立写锁"方案重构(留给 1.3-13 命令层统一提交时实施),而不是盲改次序。 */
    {
        int64_t took_ms = esp_timer_get_time() / 1000 - save_started_ms;
        if (took_ms > 50) {
            ESP_LOGW(TAG, "config save took %lld ms (config lock held)", (long long)took_ms);
        }
    }
    app_config_unlock();
    return err;
}

static inline void app_config_migrate_set_strings_from_ptr(app_config_t *config,
                                                           const char *sta_ssid,
                                                           const char *sta_password,
                                                           const char *bt_device_name,
                                                           const char *mqtt_backend,
                                                           const char *mqtt_host,
                                                           const char *mqtt_token)
{
    app_copy_string(config->sta_ssid,       sizeof(config->sta_ssid),       sta_ssid);
    app_copy_string(config->sta_password,   sizeof(config->sta_password),   sta_password);
    app_copy_string(config->bt_device_name, sizeof(config->bt_device_name), bt_device_name);
    app_copy_string(config->mqtt_backend,   sizeof(config->mqtt_backend),   mqtt_backend);
    app_copy_string(config->mqtt_host,      sizeof(config->mqtt_host),      mqtt_host);
    app_copy_string(config->mqtt_token,     sizeof(config->mqtt_token),     mqtt_token);
}

static inline void app_config_migrate_apply_common(app_config_t *config,
                                                   const char *sta_ssid,
                                                   const char *sta_password,
                                                   const char *bt_device_name,
                                                   const char *mqtt_backend,
                                                   const char *mqtt_host,
                                                   const char *mqtt_token)
{
    app_config_set_defaults(config);
    APP_MIGRATE_FIXED_FRAME();
    app_config_migrate_set_strings_from_ptr(config,
        sta_ssid, sta_password, bt_device_name, mqtt_backend, mqtt_host, mqtt_token);
    app_config_mark_current_version(config);
}

static void app_config_migrate_v3(app_config_t *config, const app_config_v3_t *legacy)
{
    app_config_migrate_apply_common(config,
        legacy->sta_ssid, legacy->sta_password,
        legacy->bt_device_name, legacy->mqtt_backend, legacy->mqtt_host, legacy->mqtt_token);
    APP_MIGRATE_COMMON_PREFIX();
}

static void app_config_migrate_v4(app_config_t *config, const app_config_v4_t *legacy)
{
    app_config_migrate_apply_common(config,
        legacy->sta_ssid, legacy->sta_password,
        legacy->bt_device_name, legacy->mqtt_backend, legacy->mqtt_host, legacy->mqtt_token);
    APP_MIGRATE_COMMON_PREFIX();
}

static void app_config_migrate_v5(app_config_t *config, const app_config_v5_t *legacy)
{
    app_config_migrate_apply_common(config,
        legacy->sta_ssid, legacy->sta_password,
        legacy->bt_device_name, legacy->mqtt_backend, legacy->mqtt_host, legacy->mqtt_token);
    APP_MIGRATE_COMMON_PREFIX();
}

void app_config_load(app_config_t *config)
{
    nvs_handle_t nvs_handle;
    size_t size = 0;

    if (config == NULL) {
        return;
    }

    app_config_set_defaults(config);
    app_config_mark_current_version(config);

    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS failed, using defaults");
        return;
    }

    if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, NULL, &size) != ESP_OK) {
        ESP_LOGI(TAG, "No compatible saved config found, storing defaults");
        app_config_set_defaults(config);
        app_config_mark_current_version(config);
        nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        return;
    }

    /* 前缀链迁移(v7+):v7/v8(544B)、v9(624B)都是当前结构体的字节前缀 ——
     * set_defaults 已把新增字段填成默认,读入前缀后按 version 补齐即可。
     * ⚠ 此前 v8→v9 分支写在 size==sizeof(*config) 里是**死代码**(v8 blob 544B
     * 永远进不了 624B 的分支,导致 v8 设备升级 v9 时被误判"无兼容配置"而重置)——
     * 这里统一改为按 blob 实际大小的前缀链处理。 */
    if (size >= offsetof(app_config_t, bt_role) && size <= sizeof(*config)) {
        size_t read_size = size;

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, config, &read_size) == ESP_OK &&
            config->magic == APP_CONFIG_MAGIC &&
            config->version >= 7 && config->version <= APP_CONFIG_VERSION) {
            if (config->version == APP_CONFIG_VERSION) {
                nvs_close(nvs_handle);
                return;
            }
            if (config->version == 7) {
                /* v7 → 当前:bt_role 由 v7 的 bt_mode 推导:1(ble) → peripheral,其余 → off */
                config->bt_role = (config->bt_mode == BT_MODE_BLE) ? BT_ROLE_PERIPHERAL : BT_ROLE_OFF;
                ESP_LOGI(TAG, "Migrating saved config from v7 to v%d (bt_role=%u)",
                         (int)APP_CONFIG_VERSION, (unsigned)config->bt_role);
            } else {
                ESP_LOGI(TAG, "Migrating saved config from v%u to v%d (appended fields keep defaults)",
                         (unsigned)config->version, (int)APP_CONFIG_VERSION);
            }
            app_config_mark_current_version(config);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    }

    if (size == sizeof(app_config_v6_t)) {
        /* v6 → v7:web 凭据字段追加在结构体末尾且 1 字节对齐,v6 blob 是 v7 的
         * 字节前缀——读入前缀,保留函数开头 set_defaults 生成的默认凭据 */
        size_t read_size = size;
        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, config, &read_size) == ESP_OK &&
            config->magic == APP_CONFIG_MAGIC &&
            config->version == 6) {
            ESP_LOGI(TAG, "Migrating saved config from v6 to v7");
            app_config_mark_current_version(config);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    } else if (size == sizeof(app_config_v5_t)) {
        app_config_v5_t legacy = {0};
        size_t legacy_size = sizeof(legacy);

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &legacy, &legacy_size) == ESP_OK &&
            legacy.magic == APP_CONFIG_MAGIC &&
            legacy.version == 5) {
            ESP_LOGI(TAG, "Migrating saved config from v5 to v6");
            app_config_migrate_v5(config, &legacy);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    } else if (size == sizeof(app_config_v4_t)) {
        app_config_v4_t legacy = {0};
        size_t legacy_size = sizeof(legacy);

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &legacy, &legacy_size) == ESP_OK &&
            legacy.magic == APP_CONFIG_MAGIC &&
            legacy.version == 4) {
            ESP_LOGI(TAG, "Migrating saved config from v4 to v6");
            app_config_migrate_v4(config, &legacy);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    } else if (size == sizeof(app_config_v3_t)) {
        app_config_v3_t legacy = {0};
        size_t legacy_size = sizeof(legacy);

        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, &legacy, &legacy_size) == ESP_OK &&
            legacy.magic == APP_CONFIG_MAGIC &&
            legacy.version == 3) {
            ESP_LOGI(TAG, "Migrating saved config from v3 to v6");
            app_config_migrate_v3(config, &legacy);
            nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
            nvs_commit(nvs_handle);
            nvs_close(nvs_handle);
            return;
        }
    }

    ESP_LOGI(TAG, "No compatible saved config found, storing defaults");
    app_config_set_defaults(config);
    app_config_mark_current_version(config);
    nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

esp_err_t app_config_load_wifi_profiles(app_wifi_profile_t *profiles, size_t max_profiles, size_t *profile_count)
{
    static app_wifi_profiles_blob_t s_blob;
    nvs_handle_t nvs_handle;
    size_t index;
    size_t copy_count = 0;

    if ((profiles == NULL && max_profiles > 0) || profile_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *profile_count = 0;
    if (max_profiles > APP_WIFI_PROFILE_MAX) {
        max_profiles = APP_WIFI_PROFILE_MAX;
    }

    app_wifi_profiles_lock();
    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        app_wifi_profiles_unlock();
        return ESP_FAIL;
    }
    app_config_load_wifi_profiles_blob(nvs_handle, &s_blob);
    nvs_close(nvs_handle);

    copy_count = s_blob.count < max_profiles ? s_blob.count : max_profiles;
    for (index = 0; index < copy_count; index++) {
        profiles[index] = s_blob.profiles[index];
    }
    *profile_count = s_blob.count;
    app_wifi_profiles_unlock();
    return ESP_OK;
}

esp_err_t app_config_save_wifi_profile(const char *ssid, const char *password)
{
    static app_wifi_profiles_blob_t s_blob;
    static app_wifi_profiles_blob_t s_old_blob;
    static app_wifi_profile_t s_existing[APP_WIFI_PROFILE_MAX - 1];
    static app_wifi_profile_t s_profile;
    nvs_handle_t nvs_handle;
    size_t index;
    size_t existing_count = 0;
    bool changed;
    esp_err_t open_err;

    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_wifi_profiles_lock();
    memset(&s_profile, 0, sizeof(s_profile));
    app_copy_string(s_profile.ssid, sizeof(s_profile.ssid), ssid);
    app_copy_string(s_profile.password, sizeof(s_profile.password), password);

    open_err = nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (open_err != ESP_OK) {
        app_wifi_profiles_unlock();
        return open_err;
    }
    app_config_load_wifi_profiles_blob(nvs_handle, &s_blob);

    /* 护栏(问题与版本优化.md 1.3-20):空口令**不得**覆盖已存的非空口令。
     * 否则一次空写(如某个调用点拿到空 profile 口令)就会永久抹掉可用凭据,
     * 且会被 IP_EVENT_STA_GOT_IP 的回写路径自我强化。 */
    if (password[0] == '\0') {
        for (index = 0; index < APP_WIFI_PROFILE_MAX; index++) {
            if (s_blob.profiles[index].ssid[0] != '\0' &&
                strcmp(s_blob.profiles[index].ssid, s_profile.ssid) == 0 &&
                s_blob.profiles[index].password[0] != '\0') {
                ESP_LOGW(TAG, "keep stored password for '%s': refuse empty overwrite",
                         s_profile.ssid);
                nvs_close(nvs_handle);
                app_wifi_profiles_unlock();
                return ESP_OK;
            }
        }
    }

    memset(s_existing, 0, sizeof(s_existing));
    existing_count = 0;
    for (index = 0; index < APP_WIFI_PROFILE_MAX; index++) {
        if (s_blob.profiles[index].ssid[0] == '\0') {
            continue;
        }
        if (strcmp(s_blob.profiles[index].ssid, s_profile.ssid) == 0) {
            continue;
        }
        if (existing_count >= APP_WIFI_PROFILE_MAX - 1) {
            break;
        }
        s_existing[existing_count++] = s_blob.profiles[index];
    }

    s_blob.profiles[0] = s_profile;
    for (index = 0; index < existing_count; index++) {
        s_blob.profiles[index + 1] = s_existing[index];
    }
    for (index = existing_count + 1; index < APP_WIFI_PROFILE_MAX; index++) {
        memset(&s_blob.profiles[index], 0, sizeof(s_blob.profiles[index]));
    }
    app_config_normalize_wifi_profiles(&s_blob);

    {
        size_t old_size = sizeof(s_old_blob);
        changed = true;
        app_config_init_wifi_profiles(&s_old_blob);
        if (nvs_get_blob(nvs_handle, APP_WIFI_PROFILES_KEY, &s_old_blob, &old_size) == ESP_OK &&
            old_size == sizeof(s_old_blob) &&
            app_config_wifi_profiles_blob_valid(&s_old_blob)) {
            if (s_old_blob.count == s_blob.count && memcmp(s_old_blob.profiles, s_blob.profiles, sizeof(s_blob.profiles)) == 0) {
                changed = false;
            }
        }
    }

    if (changed) {
        esp_err_t err = app_config_store_wifi_profiles_blob(nvs_handle, &s_blob);
        nvs_close(nvs_handle);
        if (err == ESP_OK) {
            app_wifi_mark_profiles_dirty();
        }
        app_wifi_profiles_unlock();
        return err;
    }
    nvs_close(nvs_handle);
    app_wifi_profiles_unlock();
    return ESP_OK;
}

esp_err_t app_config_delete_wifi_profile(const char *ssid)
{
    static app_wifi_profiles_blob_t s_blob;
    nvs_handle_t nvs_handle;
    size_t index;
    bool removed = false;

    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    app_wifi_profiles_lock();
    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        app_wifi_profiles_unlock();
        return ESP_FAIL;
    }
    app_config_load_wifi_profiles_blob(nvs_handle, &s_blob);
    for (index = 0; index < APP_WIFI_PROFILE_MAX; index++) {
        if (strcmp(s_blob.profiles[index].ssid, ssid) == 0) {
            memset(&s_blob.profiles[index], 0, sizeof(s_blob.profiles[index]));
            removed = true;
        }
    }
    if (!removed) {
        nvs_close(nvs_handle);
        app_wifi_profiles_unlock();
        return ESP_ERR_NOT_FOUND;
    }
    app_config_normalize_wifi_profiles(&s_blob);
    {
        esp_err_t err = app_config_store_wifi_profiles_blob(nvs_handle, &s_blob);
        nvs_close(nvs_handle);
        if (err == ESP_OK) {
            app_wifi_mark_profiles_dirty();
        }
        app_wifi_profiles_unlock();
        return err;
    }
}

esp_err_t app_config_clear_wifi_profiles(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    app_wifi_profiles_lock();
    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        app_wifi_profiles_unlock();
        return ESP_FAIL;
    }
    err = nvs_erase_key(nvs_handle, APP_WIFI_PROFILES_KEY);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;
    }
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err == ESP_OK) {
        app_wifi_mark_profiles_dirty();
    }
    app_wifi_profiles_unlock();
    return err;
}

esp_err_t app_config_snapshot_save(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    app_config_lock();
    app_wifi_profiles_lock();
    s_config_snapshot.valid = false;
    s_config_snapshot.config = s_config;
    app_config_init_wifi_profiles(&s_config_snapshot.profiles);

    err = nvs_open(APP_NAMESPACE, NVS_READONLY, &nvs_handle);
    if (err == ESP_ERR_NVS_NOT_FOUND) {
        err = ESP_OK;   /* 没有任何已保存 profile：快照为空列表 */
    } else if (err == ESP_OK) {
        err = app_config_load_wifi_profiles_blob(nvs_handle, &s_config_snapshot.profiles);
        nvs_close(nvs_handle);
    }
    if (err == ESP_OK) {
        s_config_snapshot.valid = true;
    }
    app_wifi_profiles_unlock();
    app_config_unlock();
    return err;
}

esp_err_t app_config_snapshot_restore(void)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    app_config_lock();
    app_wifi_profiles_lock();
    if (!s_config_snapshot.valid) {
        app_wifi_profiles_unlock();
        app_config_unlock();
        return ESP_ERR_INVALID_STATE;
    }

    err = app_config_save(&s_config_snapshot.config);
    if (err == ESP_OK) {
        err = nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle);
        if (err == ESP_OK) {
            err = app_config_store_wifi_profiles_blob(nvs_handle, &s_config_snapshot.profiles);
            nvs_close(nvs_handle);
        }
    }
    if (err == ESP_OK) {
        s_config_snapshot.valid = false;
        app_wifi_mark_profiles_dirty();
    }
    app_wifi_profiles_unlock();
    app_config_unlock();
    return err;
}

void app_config_snapshot_discard(void)
{
    app_config_lock();
    s_config_snapshot.valid = false;
    app_config_unlock();
}

bool app_config_load_output_last(bool relay, bool *on)
{
    nvs_handle_t nvs_handle;
    uint8_t value = 0;
    const char *key = relay ? APP_IO_RELAY_LAST_KEY : APP_IO_LED_LAST_KEY;

    if (on == NULL) {
        return false;
    }
    *on = false;
    if (nvs_open(APP_NAMESPACE, NVS_READONLY, &nvs_handle) != ESP_OK) {
        ESP_LOGW(TAG, "open NVS for %s failed, fallback off", key);
        return false;
    }
    if (nvs_get_u8(nvs_handle, key, &value) != ESP_OK) {
        nvs_close(nvs_handle);
        return false;
    }
    nvs_close(nvs_handle);
    *on = (value != 0);
    return true;
}

esp_err_t app_config_save_output_last(bool relay, bool on)
{
    nvs_handle_t nvs_handle;
    const char *key = relay ? APP_IO_RELAY_LAST_KEY : APP_IO_LED_LAST_KEY;
    esp_err_t err;

    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGW(TAG, "open NVS to save %s failed", key);
        return ESP_FAIL;
    }
    err = nvs_set_u8(nvs_handle, key, on ? 1 : 0);
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "save %s failed: %s", key, esp_err_to_name(err));
    }
    return err;
}

void app_config_clear_output_last(void)
{
    nvs_handle_t nvs_handle;

    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return;
    }
    (void)nvs_erase_key(nvs_handle, APP_IO_RELAY_LAST_KEY);
    (void)nvs_erase_key(nvs_handle, APP_IO_LED_LAST_KEY);
    (void)nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}
