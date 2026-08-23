#include <string.h>

#include "driver/uart.h"
#include "esp_check.h"
#include "esp_log.h"
#include "nvs.h"

#include "common.h"
#include "config.h"
#include "wifi.h"

#define APP_NAMESPACE "iothub"
#define APP_CONFIG_KEY "runtime_cfg"
#define APP_WIFI_PROFILES_KEY "wifi_profiles"
#define APP_CONFIG_MAGIC 0x494f5448UL
#define APP_CONFIG_VERSION 6
#define APP_WIFI_PROFILES_MAGIC 0x57494649UL
#define APP_WIFI_PROFILES_VERSION 1

typedef struct {
    uint32_t magic;
    uint16_t version;
    uint16_t count;
    app_wifi_profile_t profiles[APP_WIFI_PROFILE_MAX];
} app_wifi_profiles_blob_t;

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
    int32_t relay_gpio;
    int32_t led_gpio;
    int32_t input_gpio;
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

static const char *TAG = "iothub";

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

#define APP_MIGRATE_COMMON_STRINGS()                             \
    do {                                                         \
        APP_COPY_STR(ap_ssid);                                   \
        APP_COPY_STR(ap_password);                               \
        APP_COPY_STR(sta_ssid);                                  \
        APP_COPY_STR(sta_password);                              \
        APP_COPY_STR(bt_device_name);                            \
        APP_COPY_STR(mqtt_backend);                              \
        APP_COPY_STR(mqtt_host);                                 \
        APP_COPY_STR(mqtt_token);                                \
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
    config->version = APP_CONFIG_VERSION;
    config->relay_active_high = 1;
    config->led_active_high = 1;
    config->input_active_low = 1;
    config->bt_mode = 0;
    config->net_mode = 0;
    config->mqtt_port = 1883;
    config->uart_parity_mode = UART_PARITY_DISABLE;
    config->uart_data_bits = 8;
    config->uart_stop_bits = 1;
    config->uart_baudrate = 9600;
    app_copy_string(config->ap_ssid, sizeof(config->ap_ssid), "iothub");
    app_copy_string(config->bt_device_name, sizeof(config->bt_device_name), "iothub-bt");
    app_copy_string(config->mqtt_backend, sizeof(config->mqtt_backend), "tb_cloud");
    app_copy_string(config->mqtt_host, sizeof(config->mqtt_host), "demo.thingsboard.io");
}

esp_err_t app_config_save(const app_config_t *config)
{
    nvs_handle_t nvs_handle;
    esp_err_t err;

    if (config == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
    if (err == ESP_OK) {
        err = nvs_commit(nvs_handle);
    }
    nvs_close(nvs_handle);
    return err;
}

static inline void app_config_migrate_set_strings_from_ptr(app_config_t *config,
                                                           const char *ap_ssid,
                                                           const char *ap_password,
                                                           const char *sta_ssid,
                                                           const char *sta_password,
                                                           const char *bt_device_name,
                                                           const char *mqtt_backend,
                                                           const char *mqtt_host,
                                                           const char *mqtt_token)
{
    app_copy_string(config->ap_ssid,        sizeof(config->ap_ssid),        ap_ssid);
    app_copy_string(config->ap_password,    sizeof(config->ap_password),    ap_password);
    app_copy_string(config->sta_ssid,       sizeof(config->sta_ssid),       sta_ssid);
    app_copy_string(config->sta_password,   sizeof(config->sta_password),   sta_password);
    app_copy_string(config->bt_device_name, sizeof(config->bt_device_name), bt_device_name);
    app_copy_string(config->mqtt_backend,   sizeof(config->mqtt_backend),   mqtt_backend);
    app_copy_string(config->mqtt_host,      sizeof(config->mqtt_host),      mqtt_host);
    app_copy_string(config->mqtt_token,     sizeof(config->mqtt_token),     mqtt_token);
}

static void app_config_migrate_v3(app_config_t *config, const app_config_v3_t *legacy)
{
    app_config_set_defaults(config);
    APP_MIGRATE_COMMON_PREFIX();
    APP_MIGRATE_FIXED_FRAME();
    app_config_migrate_set_strings_from_ptr(config,
        legacy->ap_ssid, legacy->ap_password, legacy->sta_ssid, legacy->sta_password,
        legacy->bt_device_name, legacy->mqtt_backend, legacy->mqtt_host, legacy->mqtt_token);
}

static void app_config_migrate_v4(app_config_t *config, const app_config_v4_t *legacy)
{
    app_config_set_defaults(config);
    APP_MIGRATE_COMMON_PREFIX();
    APP_MIGRATE_FIXED_FRAME();
    app_config_migrate_set_strings_from_ptr(config,
        legacy->ap_ssid, legacy->ap_password, legacy->sta_ssid, legacy->sta_password,
        legacy->bt_device_name, legacy->mqtt_backend, legacy->mqtt_host, legacy->mqtt_token);
}

static void app_config_migrate_v5(app_config_t *config, const app_config_v5_t *legacy)
{
    app_config_set_defaults(config);
    APP_MIGRATE_COMMON_PREFIX();
    APP_MIGRATE_FIXED_FRAME();
    app_config_migrate_set_strings_from_ptr(config,
        legacy->ap_ssid, legacy->ap_password, legacy->sta_ssid, legacy->sta_password,
        legacy->bt_device_name, legacy->mqtt_backend, legacy->mqtt_host, legacy->mqtt_token);
}

void app_config_load(app_config_t *config)
{
    nvs_handle_t nvs_handle;
    size_t size = 0;

    if (config == NULL) {
        return;
    }

    app_config_set_defaults(config);

    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        ESP_LOGW(TAG, "Open NVS failed, using defaults");
        return;
    }

    if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, NULL, &size) != ESP_OK) {
        ESP_LOGI(TAG, "No compatible saved config found, storing defaults");
        app_config_set_defaults(config);
        nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
        nvs_commit(nvs_handle);
        nvs_close(nvs_handle);
        return;
    }

    if (size == sizeof(*config)) {
        size_t current_size = sizeof(*config);
        if (nvs_get_blob(nvs_handle, APP_CONFIG_KEY, config, &current_size) == ESP_OK &&
            config->magic == APP_CONFIG_MAGIC &&
            config->version == APP_CONFIG_VERSION) {
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
    nvs_set_blob(nvs_handle, APP_CONFIG_KEY, config, sizeof(*config));
    nvs_commit(nvs_handle);
    nvs_close(nvs_handle);
}

esp_err_t app_config_load_wifi_profiles(app_wifi_profile_t *profiles, size_t max_profiles, size_t *profile_count)
{
    nvs_handle_t nvs_handle;
    app_wifi_profiles_blob_t blob;
    size_t index;
    size_t copy_count = 0;

    if ((profiles == NULL && max_profiles > 0) || profile_count == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    *profile_count = 0;
    if (max_profiles > APP_WIFI_PROFILE_MAX) {
        max_profiles = APP_WIFI_PROFILE_MAX;
    }

    if (nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle) != ESP_OK) {
        return ESP_FAIL;
    }
    app_config_load_wifi_profiles_blob(nvs_handle, &blob);
    nvs_close(nvs_handle);

    copy_count = blob.count < max_profiles ? blob.count : max_profiles;
    for (index = 0; index < copy_count; index++) {
        profiles[index] = blob.profiles[index];
    }
    *profile_count = blob.count;
    return ESP_OK;
}

esp_err_t app_config_save_wifi_profile(const char *ssid, const char *password)
{
    nvs_handle_t nvs_handle;
    app_wifi_profiles_blob_t blob;
    size_t index;
    size_t existing_count = 0;
    app_wifi_profile_t profile = {0};
    app_wifi_profile_t existing[APP_WIFI_PROFILE_MAX - 1];

    if (ssid == NULL || ssid[0] == '\0' || password == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    app_copy_string(profile.ssid, sizeof(profile.ssid), ssid);
    app_copy_string(profile.password, sizeof(profile.password), password);

    ESP_RETURN_ON_ERROR(nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "open nvs for wifi profiles failed");
    app_config_load_wifi_profiles_blob(nvs_handle, &blob);

    for (index = 0; index < APP_WIFI_PROFILE_MAX; index++) {
        if (blob.profiles[index].ssid[0] == '\0') {
            continue;
        }
        if (strcmp(blob.profiles[index].ssid, profile.ssid) == 0) {
            continue;
        }
        if (existing_count >= APP_WIFI_PROFILE_MAX - 1) {
            break;
        }
        existing[existing_count++] = blob.profiles[index];
    }

    blob.profiles[0] = profile;
    for (index = 0; index < existing_count; index++) {
        blob.profiles[index + 1] = existing[index];
    }
    for (index = existing_count + 1; index < APP_WIFI_PROFILE_MAX; index++) {
        memset(&blob.profiles[index], 0, sizeof(blob.profiles[index]));
    }
    app_config_normalize_wifi_profiles(&blob);
    {
        esp_err_t err = app_config_store_wifi_profiles_blob(nvs_handle, &blob);
        nvs_close(nvs_handle);
        if (err == ESP_OK) {
            app_wifi_mark_profiles_dirty();
        }
        return err;
    }
}

esp_err_t app_config_delete_wifi_profile(const char *ssid)
{
    nvs_handle_t nvs_handle;
    app_wifi_profiles_blob_t blob;
    size_t index;
    bool removed = false;

    if (ssid == NULL || ssid[0] == '\0') {
        return ESP_ERR_INVALID_ARG;
    }

    ESP_RETURN_ON_ERROR(nvs_open(APP_NAMESPACE, NVS_READWRITE, &nvs_handle), TAG,
                        "open nvs for wifi profile delete failed");
    app_config_load_wifi_profiles_blob(nvs_handle, &blob);
    for (index = 0; index < APP_WIFI_PROFILE_MAX; index++) {
        if (strcmp(blob.profiles[index].ssid, ssid) == 0) {
            memset(&blob.profiles[index], 0, sizeof(blob.profiles[index]));
            removed = true;
        }
    }
    if (!removed) {
        nvs_close(nvs_handle);
        return ESP_ERR_NOT_FOUND;
    }
    app_config_normalize_wifi_profiles(&blob);
    {
        esp_err_t err = app_config_store_wifi_profiles_blob(nvs_handle, &blob);
        nvs_close(nvs_handle);
        if (err == ESP_OK) {
            app_wifi_mark_profiles_dirty();
        }
        return err;
    }
}
