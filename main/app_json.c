#include "app_json.h"

#include "cJSON.h"

static void app_json_copy_string(char *dst, size_t dst_size, const char *src)
{
    size_t index = 0;

    if (dst_size == 0) {
        return;
    }
    if (src == NULL) {
        dst[0] = '\0';
        return;
    }
    while (src[index] != '\0' && index + 1 < dst_size) {
        dst[index] = src[index];
        index++;
    }
    dst[index] = '\0';
}

static cJSON *app_json_parse_body(const char *json)
{
    if (json == NULL) {
        return NULL;
    }
    return cJSON_Parse(json);
}

bool app_json_find_string(const char *json, const char *key, char *out, size_t out_size)
{
    bool found = false;
    cJSON *root = app_json_parse_body(json);
    cJSON *item = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;

    if (out_size > 0) {
        out[0] = '\0';
    }
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        app_json_copy_string(out, out_size, item->valuestring);
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

bool app_json_find_bool(const char *json, const char *key, bool *value)
{
    bool found = false;
    cJSON *root = app_json_parse_body(json);
    cJSON *item = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;

    if (cJSON_IsBool(item)) {
        *value = cJSON_IsTrue(item);
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

bool app_json_find_u16(const char *json, const char *key, uint16_t *value)
{
    bool found = false;
    cJSON *root = app_json_parse_body(json);
    cJSON *item = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;

    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= UINT16_MAX) {
        *value = (uint16_t)item->valuedouble;
        found = true;
    }
    cJSON_Delete(root);
    return found;
}

bool app_json_find_u32(const char *json, const char *key, uint32_t *value)
{
    bool found = false;
    cJSON *root = app_json_parse_body(json);
    cJSON *item = root != NULL ? cJSON_GetObjectItemCaseSensitive(root, key) : NULL;

    if (cJSON_IsNumber(item) && item->valuedouble >= 0 && item->valuedouble <= UINT32_MAX) {
        *value = (uint32_t)item->valuedouble;
        found = true;
    }
    cJSON_Delete(root);
    return found;
}
