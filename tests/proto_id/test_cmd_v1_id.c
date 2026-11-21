#include <math.h>
#include <stdio.h>
#include <string.h>

#include "cmd_v1_id.h"

static int g_fail;
#define CHECK(cond) do { \
    if (!(cond)) { \
        g_fail++; \
        printf("FAIL %s:%d %s\n", __FILE__, __LINE__, #cond); \
    } \
} while (0)

#define SCAN(lit) app_cmd_v1_raw_has_nul_escape((lit), sizeof(lit) - 1)

static void test_reset(void)
{
    app_cmd_v1_id_t id;

    memset(&id, 0xFF, sizeof(id));
    app_cmd_v1_id_reset(&id);
    CHECK(id.kind == APP_CMD_V1_ID_NONE);
    CHECK(id.len == 0);
    CHECK(id.text[0] == '\0');
    CHECK(id.number == 0.0);

    app_cmd_v1_id_reset(NULL);
}

static void test_string_setter(void)
{
    app_cmd_v1_id_t id = {0};
    char buf[80];

    CHECK(!app_cmd_v1_id_set_string(NULL, "a"));
    CHECK(!app_cmd_v1_id_set_string(&id, NULL));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);

    CHECK(!app_cmd_v1_id_set_string(&id, ""));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);

    CHECK(app_cmd_v1_id_set_string(&id, "a"));
    CHECK(id.kind == APP_CMD_V1_ID_STRING);
    CHECK(id.len == 1);
    CHECK(strcmp(id.text, "a") == 0);

    memset(buf, 'a', 64);
    buf[64] = '\0';
    CHECK(app_cmd_v1_id_set_string(&id, buf));
    CHECK(id.kind == APP_CMD_V1_ID_STRING && id.len == 64);

    memset(buf, 'a', 65);
    buf[65] = '\0';
    CHECK(!app_cmd_v1_id_set_string(&id, buf));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);

    /* 63 字节 = 21 个 U+4E2D(3 字节/个),合法且未超限。 */
    {
        char u8[70];
        size_t i;

        for (i = 0; i < 21; i++) {
            u8[i * 3 + 0] = (char)0xE4;
            u8[i * 3 + 1] = (char)0xB8;
            u8[i * 3 + 2] = (char)0xAD;
        }
        u8[63] = '\0';
        CHECK(app_cmd_v1_id_set_string(&id, u8));
        CHECK(id.len == 63);

        /* 第 64 字节落在 3 字节字符中间 -> 截断序列。 */
        for (i = 0; i < 21; i++) {
            u8[i * 3 + 0] = (char)0xE4;
            u8[i * 3 + 1] = (char)0xB8;
            u8[i * 3 + 2] = (char)0xAD;
        }
        u8[63] = (char)0xE4;
        u8[64] = '\0';
        CHECK(!app_cmd_v1_id_set_string(&id, u8));
        CHECK(id.kind == APP_CMD_V1_ID_NONE);
    }

    /* 严格 UTF-8:overlong、代理项、超范围、孤立续字节、错误前缀。 */
    CHECK(!app_cmd_v1_id_set_string(&id, "\xC0\x80"));
    CHECK(!app_cmd_v1_id_set_string(&id, "\xED\xA0\x80"));
    CHECK(!app_cmd_v1_id_set_string(&id, "\xF4\x90\x80\x80"));
    CHECK(!app_cmd_v1_id_set_string(&id, "\xE4\xB8"));
    CHECK(!app_cmd_v1_id_set_string(&id, "\x80"));
    CHECK(!app_cmd_v1_id_set_string(&id, "\xF8\x88\x80\x80\x80"));

    CHECK(app_cmd_v1_id_set_string(&id, "\xF0\x9F\x98\x80"));   /* U+1F600 */
    CHECK(id.len == 4);
}

static void test_number_setter(void)
{
    app_cmd_v1_id_t id = {0};

    CHECK(!app_cmd_v1_id_set_number(NULL, 1.0));

    CHECK(app_cmd_v1_id_set_number(&id, 12.5));
    CHECK(id.kind == APP_CMD_V1_ID_NUMBER);
    CHECK(id.number == 12.5);

    /* 1e300 是有限 double:旧实现 (long long) 强转是 UB,新实现必须接受。 */
    CHECK(app_cmd_v1_id_set_number(&id, 1e300));
    CHECK(id.kind == APP_CMD_V1_ID_NUMBER);
    CHECK(id.number == 1e300);

    CHECK(!app_cmd_v1_id_set_number(&id, NAN));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);
    CHECK(!app_cmd_v1_id_set_number(&id, INFINITY));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);
    CHECK(!app_cmd_v1_id_set_number(&id, -INFINITY));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);

    CHECK(app_cmd_v1_id_set_number(&id, -0.0));
    CHECK(id.kind == APP_CMD_V1_ID_NUMBER);

    /* 失败前已成功写入的字符串不得残留。 */
    CHECK(app_cmd_v1_id_set_string(&id, "keep"));
    CHECK(!app_cmd_v1_id_set_number(&id, INFINITY));
    CHECK(id.kind == APP_CMD_V1_ID_NONE);
    CHECK(id.len == 0);
    CHECK(id.text[0] == '\0');
}

static void test_scanner(void)
{
    CHECK(app_cmd_v1_raw_has_nul_escape(NULL, 0) == false);
    CHECK(app_cmd_v1_raw_has_nul_escape("", 0) == false);

    /* 命中:值、键、嵌套。 */
    CHECK(SCAN("\"a\\u0000b\""));
    CHECK(SCAN("{\"\\u0000\":1}"));
    CHECK(SCAN("{\"k\":\"\\u0000\"}"));
    CHECK(SCAN("{\"a\":{\"b\":[\"x\",\"\\u0000\"]}}"));

    /* 不命中:字面反斜杠 + u0000。 */
    {
        const char literal[] = {'"', '\\', '\\', 'u', '0', '0', '0', '0', '"'};
        CHECK(app_cmd_v1_raw_has_nul_escape(literal, sizeof(literal)) == false);
    }
    CHECK(!SCAN("\"a\\\\u0000b\""));

    /* 反斜杠计数:3 个反斜杠 = 1 个转义反斜杠 + 真正的 \u0000。 */
    {
        const char mixed[] = {'"', '\\', '\\', '\\', 'u', '0', '0', '0', '0', '"'};
        CHECK(app_cmd_v1_raw_has_nul_escape(mixed, sizeof(mixed)));
    }

    /* 其他合法转义不误判;非零 \u 不命中;非法/截断转义不命中。 */
    CHECK(!SCAN("\"\\n\\t\\\"\\\\\""));
    CHECK(!SCAN("{\"k\":\"\\u0001\"}"));
    CHECK(!SCAN("{\"k\":\"\\u000A\"}"));
    CHECK(!SCAN("{\"k\":\"\\u000G\"}"));
    CHECK(!SCAN("{\"k\":\"\\u00\"}"));
    CHECK(!SCAN("\"\\U0000\""));

    /* 字符串外的 \u0000 不参与匹配。 */
    CHECK(!SCAN("\\u0000"));

    /* 边界:无 NUL 终止符的缓冲区,命中且不越界。 */
    {
        char raw[8] = {'"', '\\', 'u', '0', '0', '0', '0', '"'};
        CHECK(app_cmd_v1_raw_has_nul_escape(raw, sizeof(raw)));
    }
    /* 边界:转义在缓冲区末尾被截断。 */
    {
        char trunc[5] = {'"', '\\', 'u', '0', '0'};
        CHECK(app_cmd_v1_raw_has_nul_escape(trunc, sizeof(trunc)) == false);
    }
    {
        char backslash_end[2] = {'"', '\\'};
        CHECK(app_cmd_v1_raw_has_nul_escape(backslash_end, sizeof(backslash_end)) == false);
    }
}

int main(void)
{
    test_reset();
    test_string_setter();
    test_number_setter();
    test_scanner();

    printf("proto_id: %s\n", g_fail == 0 ? "PASS" : "FAIL");
    return g_fail == 0 ? 0 : 1;
}
