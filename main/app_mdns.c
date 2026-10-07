#include <ctype.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "mdns.h"

#include "app_mdns.h"
#include "common.h"
#include "config.h"

static const char *TAG = "app_mdns";

extern app_config_t s_config;

/* 主机名随 AP 名:IotHub-6BBC → iothub-6bbc.local。SSID 中非 [a-z0-9-] 字符
 * 统一折叠为 '-',mDNS 标签上限 63 字符 */
static void app_mdns_build_hostname(char *out, size_t out_size)
{
    size_t j = 0;

    app_config_lock();
    app_copy_string(out, out_size, s_config.ap_ssid);
    app_config_unlock();

    for (size_t i = 0; out[i] != '\0' && j + 1 < out_size; i++) {
        unsigned char ch = (unsigned char)out[i];
        if (isalnum(ch)) {
            out[j++] = (char)tolower(ch);
        } else if (j > 0 && out[j - 1] != '-') {
            out[j++] = '-';
        }
    }
    out[j] = '\0';
    while (j > 0 && out[j - 1] == '-') {
        out[--j] = '\0';
    }
    if (out[0] == '\0') {
        snprintf(out, out_size, "iothub");
    }
}

void app_mdns_start(void)
{
    char hostname[64];
    esp_err_t err;

    err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns init skipped: %s", esp_err_to_name(err));
        return;
    }

    app_mdns_build_hostname(hostname, sizeof(hostname));
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns hostname set failed: %s", esp_err_to_name(err));
        mdns_free();
        return;
    }

    err = mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "mdns service add failed: %s", esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "mDNS ready: http://%s.local", hostname);
}
