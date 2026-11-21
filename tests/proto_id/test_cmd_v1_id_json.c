/* PROTO-ID-01 / M8：v1 请求 id 的**端到端序列化**测试。
 *
 * 与 test_cmd_v1_id.c 的区别：那一份测纯 setter/scanner（不依赖 cJSON），
 * 本份把真的 cmd_v1_id_json.c 拉进来，走 app_cmd_v1_id_from_root() →
 * app_cmd_v1_id_add() → cJSON_PrintUnformatted()，断言的是**最终 JSON 文本**
 * 和**再解析后的 double**，而不是 snprintf 的中间结果。
 *
 * 背景（M8）：cJSON print_number() 先试 "%1.15g"，再用 compare_double()
 * （容差 max*DBL_EPSILON）判断能否无损回读。2^53 时 15g 回读误差 = 2、
 * 容差也 = 2，`<=` 成立 → 误判，输出 9007199254740990。本用例锁死该边界。
 *
 * 依赖：只链 cmd_v1_id.c + cmd_v1_id_json.c + cJSON；不需要命令内核，
 * 也不需要任何链接桩（这正是把它拆成独立文件的目的）。
 */
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cmd_v1_id.h"
#include "cmd_v1_id_json.h"
#include "cJSON.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        g_fail++; \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

/* ---- 用例 ---- */

/* 从请求 JSON 取 id → 写入响应 → 序列化文本必须等于 expect_text，
 * 且再解析后的 id 必须是 number 且数值等于 expect_val。 */
static void check_case(const char *req_json, const char *expect_text, double expect_val)
{
    cJSON *req = cJSON_Parse(req_json);
    cJSON *out;
    app_cmd_v1_id_t id;
    char *text;
    cJSON *parsed;
    cJSON *idnode;
    char want[96];

    CHECK(req != NULL);
    if (req == NULL) {
        return;
    }
    app_cmd_v1_id_reset(&id);
    CHECK(app_cmd_v1_id_from_root(req, &id));
    CHECK(id.kind == APP_CMD_V1_ID_NUMBER);

    out = cJSON_CreateObject();
    CHECK(out != NULL);
    CHECK(app_cmd_v1_id_add(out, &id));

    text = cJSON_PrintUnformatted(out);
    CHECK(text != NULL);
    if (text != NULL) {
        snprintf(want, sizeof(want), "{\"id\":%s}", expect_text);
        if (strcmp(text, want) != 0) {
            g_fail++;
            printf("FAIL text: got %s want %s (req %s)\n", text, want, req_json);
        }
        /* 必须是 JSON number，不能被引号包成字符串 */
        CHECK(strstr(text, "\"id\":\"") == NULL);

        parsed = cJSON_Parse(text);
        CHECK(parsed != NULL);
        idnode = parsed ? cJSON_GetObjectItemCaseSensitive(parsed, "id") : NULL;
        CHECK(cJSON_IsNumber(idnode));
        if (idnode != NULL && idnode->valuedouble != expect_val) {
            g_fail++;
            printf("FAIL value: got %.17g want %.17g (req %s)\n",
                   idnode->valuedouble, expect_val, req_json);
        }
        cJSON_Delete(parsed);
        cJSON_free(text);
    }
    cJSON_Delete(out);
    cJSON_Delete(req);
}

static void test_number_serialization(void)
{
    /* 2^53+1 解析后即 2^53；修复前会被打印成 9007199254740990 */
    check_case("{\"id\":9007199254740993}", "9007199254740992", 9007199254740992.0);
    check_case("{\"id\":9007199254740992}", "9007199254740992", 9007199254740992.0);
    check_case("{\"id\":-9007199254740992}", "-9007199254740992", -9007199254740992.0);
    check_case("{\"id\":2147483648}", "2147483648", 2147483648.0);
    check_case("{\"id\":0}", "0", 0.0);
    check_case("{\"id\":1.5}", "1.5", 1.5);
    /* 小数量：文本不要求与输入字面一致，但必须往返成同一个 double
     * （1e-7 的 17 位十进制是 9.9999999999999995e-08） */
    check_case("{\"id\":1e-7}", "9.9999999999999995e-08", 1e-7);
}

static void test_string_unchanged(void)
{
    cJSON *req = cJSON_Parse("{\"id\":\"mp-1\"}");
    cJSON *out = cJSON_CreateObject();
    app_cmd_v1_id_t id;
    char *text;

    app_cmd_v1_id_reset(&id);
    CHECK(req != NULL && out != NULL);
    CHECK(app_cmd_v1_id_from_root(req, &id));
    CHECK(id.kind == APP_CMD_V1_ID_STRING);
    CHECK(app_cmd_v1_id_add(out, &id));
    text = cJSON_PrintUnformatted(out);
    CHECK(text != NULL);
    CHECK(text != NULL && strcmp(text, "{\"id\":\"mp-1\"}") == 0);
    if (text != NULL) {
        cJSON_free(text);
    }
    cJSON_Delete(out);
    cJSON_Delete(req);
}

int main(void)
{
    test_number_serialization();
    test_string_unchanged();

    printf("proto_id_json: %s (%d failures)\n", g_fail == 0 ? "PASS" : "FAIL", g_fail);
    return g_fail == 0 ? 0 : 1;
}
