#include "cmd_v1_id_json.h"

#include <math.h>
#include <stddef.h>
#include <stdio.h>

bool app_cmd_v1_id_from_root(const cJSON *root, app_cmd_v1_id_t *out)
{
    const cJSON *item;

    if (out == NULL) {
        return false;
    }
    app_cmd_v1_id_reset(out);
    if (root == NULL) {
        return false;
    }
    item = cJSON_GetObjectItemCaseSensitive(root, "id");
    if (cJSON_IsString(item) && item->valuestring != NULL) {
        return app_cmd_v1_id_set_string(out, item->valuestring);
    }
    if (cJSON_IsNumber(item)) {
        return app_cmd_v1_id_set_number(out, item->valuedouble);
    }
    return false;
}

bool app_cmd_v1_id_add(cJSON *root, const app_cmd_v1_id_t *id)
{
    cJSON *added;

    if (root == NULL || id == NULL) {
        return false;
    }
    switch (id->kind) {
    case APP_CMD_V1_ID_STRING:
        added = cJSON_AddStringToObject(root, "id", id->text);
        break;
    case APP_CMD_V1_ID_NUMBER: {
        /* M8（PROTO-ID-01，2026-11-22）：cJSON print_number() 先试 "%1.15g"，
         * 再用 compare_double()（容差 max*DBL_EPSILON）判断能否无损回读。
         * 2^53 时 15g 回读误差 = 2、容差也 = 2，`<=` 成立 → 误判"足够精确"，
         * 输出 9007199254740990（少 2）。这里改用 "%.17g" 生成往返无损文本，
         * 以 JSON number 原样写入；任一步失败回退 cJSON 默认打印，不丢 id。
         * id->number 已由 app_cmd_v1_id_set_number() 保证有限；仍防御一次，
         * 绝不把 inf/nan 文本交给 AddRaw（那会产出非法 JSON）。
         *
         * locale 前提（评审 2026-11-22 提出）：JSON 小数点必须是 '.'，而
         * snprintf("%.17g") 受当前 C locale 影响。ESP-IDF 默认 C locale，
         * 本工程未调用 setlocale()/localeconv()（全仓 grep 为空），故文本恒为
         * '.'。若将来引入数值 locale，必须在此把 locale 小数点替换成 '.' 后再
         * AddRaw——cJSON print_number() 正是用 get_decimal_point() 做的同样处理。 */
        char num[32];
        int n = -1;

        if (isfinite(id->number)) {
            n = snprintf(num, sizeof(num), "%.17g", id->number);
        }
        if (n > 0 && (size_t)n < sizeof(num)) {
            added = cJSON_AddRawToObject(root, "id", num);
        } else {
            added = NULL;
        }
        if (added == NULL) {
            added = cJSON_AddNumberToObject(root, "id", id->number);
        }
        break;
    }
    case APP_CMD_V1_ID_NONE:
        return true;
    default:
        return false;
    }
    return added != NULL;
}
