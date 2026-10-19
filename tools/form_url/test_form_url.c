/* 宿主单测:main/form_url.c(表单解码 + Cookie 解析)纯逻辑验证。
 * 运行:
 *   gcc -Wall -Wextra -I main -o /tmp/t_form_url \
 *       tools/form_url/test_form_url.c main/form_url.c && /tmp/t_form_url
 *   期望:结果: 共 N 例, 失败 0
 *
 * 为什么重要:门户登录(POST /api/v1/auth/login)与会话 Cookie 的正确性完全依赖这两段
 * 解析;真机上出错往往只是"偶发登录失败",很难定位。见 问题与版本优化.md 1.3-01。 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include "form_url.h"

static int s_total;
static int s_failed;

static void check_str(const char *name, const char *got, const char *want)
{
    s_total++;
    if (strcmp(got, want) != 0) {
        s_failed++;
        printf("  [FAIL] %s: got '%s' want '%s'\n", name, got, want);
    }
}

static void check_bool(const char *name, bool got, bool want)
{
    s_total++;
    if (got != want) {
        s_failed++;
        printf("  [FAIL] %s: got %d want %d\n", name, (int)got, (int)want);
    }
}

static void check_decode(const char *name, const char *in, const char *want)
{
    char buf[128];

    snprintf(buf, sizeof(buf), "%s", in);
    app_url_decode(buf);
    check_str(name, buf, want);
}

int main(void)
{
    char form1[] = "username=admin&password=123456";
    char form2[] = "password=a%3Db&username=ad%20min";
    char form3[] = "username=&password=x";
    char form4[] = "password=only";
    char form5[] = "username=admin&username=dup";
    char out[64];

    printf("form_url 表单/Cookie 解析单测\n");

    /* ---- urlencoded 解码 ---- */
    check_decode("plus-space", "a+b", "a b");
    check_decode("percent-abc", "%41%42%43", "ABC");
    check_decode("invalid-pct", "a%2Gb", "a%2Gb");     /* 非十六进制 → 原样 */
    check_decode("short-pct", "a%4", "a%4");           /* 不足两位 → 原样 */
    check_decode("double-pct", "%%", "%%");            /* 第二位非法 → 原样 */
    check_decode("mixed-space", "+%20+", "   ");
    check_decode("utf8", "%E4%BD%A0", "你");
    check_decode("pct-zero-truncates", "a%00b", "a");  /* %00:字符串在此截断 */
    check_decode("no-change", "plain", "plain");

    /* ---- 表单字段 ---- */
    app_form_decode_all(form1);
    check_bool("f1-user-found", app_form_lookup(form1, "username", out, sizeof(out)), true);
    check_str("f1-user-value", out, "admin");
    check_bool("f1-pass-found", app_form_lookup(form1, "password", out, sizeof(out)), true);
    check_str("f1-pass-value", out, "123456");
    check_bool("f1-missing", app_form_lookup(form1, "nope", out, sizeof(out)), false);

    app_form_decode_all(form2);
    check_bool("f2-user-found", app_form_lookup(form2, "username", out, sizeof(out)), true);
    check_str("f2-user-value", out, "ad min");
    check_bool("f2-pass-found", app_form_lookup(form2, "password", out, sizeof(out)), true);
    check_str("f2-pass-value", out, "a=b");            /* 值里含 '=' 要保留 */

    app_form_decode_all(form3);
    check_bool("f3-empty-found", app_form_lookup(form3, "username", out, sizeof(out)), true);
    check_str("f3-empty-value", out, "");

    app_form_decode_all(form4);
    check_bool("f4-pass-found", app_form_lookup(form4, "password", out, sizeof(out)), true);
    check_bool("f4-user-absent", app_form_lookup(form4, "username", out, sizeof(out)), false);

    app_form_decode_all(form5);
    check_bool("f5-dup-first-wins", app_form_lookup(form5, "username", out, sizeof(out)), true);
    check_str("f5-dup-value", out, "admin");

    /* ---- 表单字段(**先切字段再解码**:新推荐入口 app_form_lookup_raw)----
     * 关键回归:值里含 `%26`(&) / `%2B`(+) 时必须**完整取出**。
     * 旧组合(decode_all + lookup)会先把 %26 解成裸 '&',再当分隔符切一刀,
     * 把值截断成前半段 —— 实测用户名为 `<b>"x&y` 时只收到 `<b>"x`(5 字符),永远登不进去。 */
    {
        char r1[] = "username=admin&password=123456";
        char r2[] = "password=a%26b%2Bc&username=a%26b";
        char r3[] = "username=%3Cb%3E%22x%26y&password=123456";
        char r4[] = "username=ad+min&password=x";
        char r5[] = "password=only";
        char r6[] = "username=admin&username=dup";

        check_bool("raw1-user", app_form_lookup_raw(r1, "username", out, sizeof(out)), true);
        check_str("raw1-user-value", out, "admin");
        check_bool("raw1-pass", app_form_lookup_raw(r1, "password", out, sizeof(out)), true);
        check_str("raw1-pass-value", out, "123456");
        check_bool("raw1-missing", app_form_lookup_raw(r1, "nope", out, sizeof(out)), false);

        check_bool("raw2-user-amp", app_form_lookup_raw(r2, "username", out, sizeof(out)), true);
        check_str("raw2-user-amp-value", out, "a&b");          /* ★ 不得被截断成 "a" */
        check_bool("raw2-pass-amp-plus", app_form_lookup_raw(r2, "password", out, sizeof(out)), true);
        check_str("raw2-pass-amp-plus-value", out, "a&b+c");

        check_bool("raw3-inject-user", app_form_lookup_raw(r3, "username", out, sizeof(out)), true);
        check_str("raw3-inject-user-value", out, "<b>\"x&y");  /* ★ 本次真机踩到的串 */
        check_bool("raw3-pass", app_form_lookup_raw(r3, "password", out, sizeof(out)), true);
        check_str("raw3-pass-value", out, "123456");

        check_bool("raw4-plus-space", app_form_lookup_raw(r4, "username", out, sizeof(out)), true);
        check_str("raw4-plus-space-value", out, "ad min");

        check_bool("raw5-absent", app_form_lookup_raw(r5, "username", out, sizeof(out)), false);
        check_bool("raw6-dup-first", app_form_lookup_raw(r6, "username", out, sizeof(out)), true);
        check_str("raw6-dup-value", out, "admin");
    }

    /* ---- Cookie 解析 ---- */
    check_bool("ck-exact", app_cookie_value("iothub_sid=abc123", "iothub_sid", out, sizeof(out)), true);
    check_str("ck-exact-value", out, "abc123");
    check_bool("ck-among", app_cookie_value("a=1; iothub_sid=xyz; b=2", "iothub_sid", out, sizeof(out)), true);
    check_str("ck-among-value", out, "xyz");
    check_bool("ck-prefix-trap", app_cookie_value("xiothub_sid=zz", "iothub_sid", out, sizeof(out)), false);
    check_bool("ck-no-space", app_cookie_value("a=1;iothub_sid=q", "iothub_sid", out, sizeof(out)), true);
    check_str("ck-no-space-value", out, "q");
    check_bool("ck-trailing-semi", app_cookie_value("iothub_sid=v1;", "iothub_sid", out, sizeof(out)), true);
    check_str("ck-trailing-semi-value", out, "v1");
    check_bool("ck-missing", app_cookie_value("a=1; b=2", "iothub_sid", out, sizeof(out)), false);
    check_bool("ck-empty-value", app_cookie_value("iothub_sid=", "iothub_sid", out, sizeof(out)), false);
    check_bool("ck-null-header", app_cookie_value(NULL, "iothub_sid", out, sizeof(out)), false);

    printf("结果: 共 %d 例, 失败 %d\n", s_total, s_failed);
    return s_failed == 0 ? 0 : 1;
}
