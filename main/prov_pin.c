#include "prov_pin.h"

#include <string.h>

#include "nvs.h"

#define PROV_PIN_NAMESPACE "ble_prov"
#define PROV_PIN_KEY       "pop"

bool app_prov_pin_is_valid(const char *pin)
{
    size_t i;
    size_t len;

    if (pin == NULL) {
        return false;
    }
    len = strlen(pin);
    if (len < 8 || len > APP_PROV_PIN_MAX_LEN) {
        return false;
    }
    for (i = 0; i < len; i++) {
        unsigned char c = (unsigned char)pin[i];

        if (c < 0x20 || c > 0x7e) {
            return false;
        }
    }
    return true;
}

esp_err_t app_prov_pin_get(char *out, size_t out_len)
{
    char stored[APP_PROV_PIN_MAX_LEN + 1];
    size_t stored_len = sizeof(stored);
    nvs_handle_t handle;
    esp_err_t err;

    if (out == NULL || out_len < sizeof(APP_PROV_PIN_DEFAULT)) {
        return ESP_ERR_INVALID_ARG;
    }

    err = nvs_open(PROV_PIN_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        err = nvs_get_str(handle, PROV_PIN_KEY, stored, &stored_len);
        nvs_close(handle);
        if (err == ESP_OK && app_prov_pin_is_valid(stored)) {
            if (strlen(stored) + 1 > out_len) {
                return ESP_ERR_INVALID_SIZE;
            }
            strcpy(out, stored);
            return ESP_OK;
        }
    }

    strcpy(out, APP_PROV_PIN_DEFAULT);
    return ESP_OK;
}

esp_err_t app_prov_pin_set(const char *pin)
{
    nvs_handle_t handle;
    esp_err_t err;

    if (!app_prov_pin_is_valid(pin)) {
        return ESP_ERR_INVALID_ARG;
    }
    err = nvs_open(PROV_PIN_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) {
        return err;
    }
    err = nvs_set_str(handle, PROV_PIN_KEY, pin);
    if (err == ESP_OK) {
        err = nvs_commit(handle);
    }
    nvs_close(handle);
    return err;
}
