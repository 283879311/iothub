/* ============================================================================
 * token store 单测(验收顺序第 0 项)
 *
 * 只测 auth_token.c 这一层,不经 ble_cmd / cmd。目的是在接入任何通道之前
 * 先把"槽位隔离 + TTL 边界 + 常量时间比较 + 失败不清槽"四条地基钉死——
 * 这些性质一旦在集成后才发现问题,现场看到的只会是"小程序登录莫名失败"。
 *
 * 编译:make -C tests/token_store
 * 运行:make -C tests/token_store run
 * ========================================================================== */
#include <pthread.h>
#include <stdio.h>
#include <string.h>

#include "auth.h"
#include "esp_random.h"
#include "esp_timer.h"

/* ---------------------------------------------------------------- 断言框架 */
static int g_pass = 0;
static int g_fail = 0;
static int g_group_fail = 0;

static void check(int cond, const char *expr, const char *file, int line, const char *note)
{
    if (cond) {
        g_pass++;
        return;
    }
    g_fail++;
    g_group_fail++;
    printf("  FAIL  %s:%d  %s", file, line, expr);
    if (note != NULL && note[0] != '\0') {
        printf("   [%s]", note);
    }
    printf("\n");
}

#define CHECK(cond)              check((cond), #cond, __FILE__, __LINE__, "")
#define CHECK_MSG(cond, note)    check((cond), #cond, __FILE__, __LINE__, (note))

static void group(const char *name)
{
    g_group_fail = 0;
    printf("\n[%s]\n", name);
}

static void group_end(void)
{
    if (g_group_fail == 0) {
        printf("  ok\n");
    }
}

/* 32 位十六进制校验,用于反向断言 out 的格式 */
static int is_32_hex_upper(const char *s)
{
    int i;
    if (s == NULL || strlen(s) != APP_TOKEN_HEX_LEN) {
        return 0;
    }
    for (i = 0; i < APP_TOKEN_HEX_LEN; i++) {
        char c = s[i];
        if (!((c >= '0' && c <= '9') || (c >= 'A' && c <= 'F'))) {
            return 0;
        }
    }
    return 1;
}

/* 造一个格式合法但不属于任何槽位的 token(用于"错 token 不清槽") */
static void make_foreign_token(char *out)
{
    static const char kHex[] = "0123456789ABCDEF";
    uint8_t raw[APP_TOKEN_HEX_LEN / 2];
    size_t  i;

    memset(raw, 0xAB, sizeof(raw)); /* 固定值,避免随机偶合 */
    for (i = 0; i < sizeof(raw); i++) {
        out[i * 2]     = kHex[raw[i] >> 4];
        out[i * 2 + 1] = kHex[raw[i] & 0x0f];
    }
    out[APP_TOKEN_HEX_LEN] = '\0';
}

/* ============================================================ 1. 签发与校验 */
static void t_issue_validate(void)
{
    char a[APP_TOKEN_HEX_LEN + 1];
    char b[APP_TOKEN_HEX_LEN + 1];

    group("1. 签发与校验");

    memset(a, 0, sizeof(a));
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, a, sizeof(a)));
    CHECK_MSG(is_32_hex_upper(a), a);
    CHECK_MSG(strlen(a) == APP_TOKEN_HEX_LEN, "必须是 32 字符,不是 31 或 33");
    CHECK(app_token_store_validate(TOKEN_SRC_BLE, a));

    /* 两次签发的 token 必须不同(16 字节随机,碰撞概率可忽略;
     * 若这里失败,说明随机源没接上或编码写死) */
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, b, sizeof(b)));
    CHECK(strcmp(a, b) != 0);

    group_end();
}

/* ====================================================== 2. 重签后旧 token 失效 */
static void t_reissue_invalidates_old(void)
{
    char first[APP_TOKEN_HEX_LEN + 1];
    char second[APP_TOKEN_HEX_LEN + 1];

    group("2. 重签使旧 token 失效");

    CHECK(app_token_store_issue(TOKEN_SRC_BLE, first, sizeof(first)));
    CHECK(app_token_store_validate(TOKEN_SRC_BLE, first));

    CHECK(app_token_store_issue(TOKEN_SRC_BLE, second, sizeof(second)));
    CHECK_MSG(!app_token_store_validate(TOKEN_SRC_BLE, first),
              "旧 token 必须立刻失效,不能并存");
    CHECK(app_token_store_validate(TOKEN_SRC_BLE, second));

    group_end();
}

/* ============================================================ 3. 参数与越界 */
static void t_bad_args(void)
{
    char buf[APP_TOKEN_HEX_LEN + 1];
    char small[APP_TOKEN_HEX_LEN]; /* 32 字节,装不下 NUL */
    char keep[APP_TOKEN_HEX_LEN + 1];

    group("3. 参数校验与 source 越界");

    CHECK(!app_token_store_issue(TOKEN_SRC_BLE, NULL, 64));
    CHECK_MSG(!app_token_store_issue(TOKEN_SRC_BLE, small, sizeof(small)),
              "out_size < 33 必须拒绝,否则 NUL 写不进去");
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, buf, APP_TOKEN_HEX_LEN + 1));

    /* 已签发有效 token 后,失败调用不得破坏槽位 */
    strcpy(keep, buf);
    CHECK(!app_token_store_issue(TOKEN_SRC_BLE, small, sizeof(small)));
    CHECK(!app_token_store_issue(TOKEN_SRC_BLE, NULL, 64));
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_BLE, keep),
              "失败的 issue 不得清掉有效 token");

    /* 越界 source:枚举外、负值、超大值,一律 false 且不得踩坏已有槽位 */
    CHECK(!app_token_store_issue(TOKEN_SRC_MAX, buf, sizeof(buf)));
    CHECK(!app_token_store_issue((token_source_t)99, buf, sizeof(buf)));
    CHECK(!app_token_store_issue((token_source_t)-1, buf, sizeof(buf)));
    CHECK_MSG(!app_token_store_validate(TOKEN_SRC_MAX, keep), "越界 source 不得读越界槽位");
    CHECK_MSG(!app_token_store_validate((token_source_t)-1, keep), "负值 source 必须挡住");

    /* 失败调用不应破坏已签发的 token */
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_BLE, keep),
              "失败的 issue/validate 不得清掉有效 token");

    /* clear 越界只应无害返回 */
    app_token_store_clear(TOKEN_SRC_MAX);
    app_token_store_clear((token_source_t)-1);
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_BLE, keep), "越界 clear 不得碰到 BLE 槽");

    group_end();
}

/* ======================================================== 4. 格式检查(锁外) */
static void t_format_check(void)
{
    char tok[APP_TOKEN_HEX_LEN + 1];
    char lower[APP_TOKEN_HEX_LEN + 1];
    char longtok[APP_TOKEN_HEX_LEN + 3];
    char shorttok[APP_TOKEN_HEX_LEN];
    int  i;

    group("4. 格式检查");

    CHECK(app_token_store_issue(TOKEN_SRC_BLE, tok, sizeof(tok)));

    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, NULL));
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, ""));

    /* 31 字符 */
    memcpy(shorttok, tok, APP_TOKEN_HEX_LEN - 1);
    shorttok[APP_TOKEN_HEX_LEN - 1] = '\0';
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, shorttok));

    /* 33 字符(合法前缀 + 多余字符) */
    memcpy(longtok, tok, APP_TOKEN_HEX_LEN);
    longtok[APP_TOKEN_HEX_LEN]     = 'A';
    longtok[APP_TOKEN_HEX_LEN + 1] = '\0';
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, longtok));

    /* 非十六进制 */
    memcpy(longtok, tok, sizeof(tok));
    longtok[7] = 'G';
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, longtok));
    longtok[7] = ' ';
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, longtok));
    longtok[7] = '\0'; /* 中间截断 */
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, longtok));

    /* 小写形式:格式合法,但槽位存的是大写,逐字节比较必须不通过。
     * 这条是刻意的——token 是不透明字符串,不做大小写归一。 */
    for (i = 0; i <= APP_TOKEN_HEX_LEN; i++) {
        char c = tok[i];
        lower[i] = (c >= 'A' && c <= 'F') ? (char)(c - 'A' + 'a') : c;
    }
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, lower));

    /* 确认上面那些失败没有顺手把好 token 清掉 */
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_BLE, tok),
              "格式非法只是拒绝,不得清槽");

    group_end();
}

/* ============================================================== 5. TTL 边界 */
static void t_expiry(void)
{
    char tok[APP_TOKEN_HEX_LEN + 1];
    int64_t t0;

    group("5. TTL 边界(>= 语义)");

    t0 = 1000000;
    test_clock_set_ms(t0);
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, tok, sizeof(tok)));

    /* 到期前 1ms 仍有效 */
    test_clock_set_ms(t0 + APP_TOKEN_TTL_MS - 1);
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_BLE, tok), "TTL-1ms 应仍有效");

    /* 恰好到点即失效:用 '>' 的实现会在这里错判为有效 */
    test_clock_set_ms(t0 + APP_TOKEN_TTL_MS);
    CHECK_MSG(!app_token_store_validate(TOKEN_SRC_BLE, tok),
              "now == expire 必须失效(>= 而不是 >)");

    /* 过期后槽位应已被清理:同一 token 再验一次仍 false */
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, tok));

    /* 过期后重新签发,新 token 立刻可用(时钟回到有效区间内) */
    test_clock_set_ms(t0);
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, tok, sizeof(tok)));
    CHECK(app_token_store_validate(TOKEN_SRC_BLE, tok));

    group_end();
}

/* ===================================================== 6. 清槽语义与槽位隔离 */
static void t_clear_and_isolation(void)
{
    char ble[APP_TOKEN_HEX_LEN + 1];
    char ser[APP_TOKEN_HEX_LEN + 1];
    char foreign[APP_TOKEN_HEX_LEN + 1];

    group("6. 清槽语义与来源隔离");

    test_clock_set_ms(2000000);
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, ble, sizeof(ble)));
    CHECK(app_token_store_issue(TOKEN_SRC_SERIAL, ser, sizeof(ser)));

    /* 两槽独立:互不覆盖,内容不同 */
    CHECK_MSG(strcmp(ble, ser) != 0, "两个来源必须是独立随机值");
    CHECK(app_token_store_validate(TOKEN_SRC_BLE, ble));
    CHECK(app_token_store_validate(TOKEN_SRC_SERIAL, ser));

    /* 串口 token 不能当 BLE token 用(跨槽校验必须失败) */
    CHECK_MSG(!app_token_store_validate(TOKEN_SRC_BLE, ser), "跨槽 token 不得通过");
    CHECK_MSG(!app_token_store_validate(TOKEN_SRC_SERIAL, ble), "跨槽 token 不得通过");

    /* 错 token 不得清槽:一个错误 token 不该能把有效会话踢下线 */
    make_foreign_token(foreign);
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, foreign));
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_BLE, ble),
              "错误 token 校验失败后,有效 token 必须仍然有效");

    /* clear 只清指定来源 */
    app_token_store_clear(TOKEN_SRC_BLE);
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, ble));
    CHECK_MSG(app_token_store_validate(TOKEN_SRC_SERIAL, ser),
              "clear(BLE) 不得影响 SERIAL 槽");

    /* clear_all 清全部(改密路径) */
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, ble, sizeof(ble)));
    app_token_store_clear_all();
    CHECK(!app_token_store_validate(TOKEN_SRC_BLE, ble));
    CHECK_MSG(!app_token_store_validate(TOKEN_SRC_SERIAL, ser),
              "clear_all 后 SERIAL 槽也必须为空");

    group_end();
}

/* ========================================================= 7. 并发冒烟 */
#define CONC_THREADS 4
#define CONC_ITERS   20000

typedef struct {
    token_source_t src;
    int            true_hits;
    int            issues;
} conc_arg_t;

static void *conc_worker(void *p)
{
    conc_arg_t *a = (conc_arg_t *)p;
    char        tok[APP_TOKEN_HEX_LEN + 1];
    int         i;

    for (i = 0; i < CONC_ITERS; i++) {
        if (app_token_store_issue(a->src, tok, sizeof(tok))) {
            a->issues++;
            /* 同一来源只有本线程写,所以刚签发的 token 必然仍有效 ——
             * 若锁写漏(槽位被撕裂),这里会间歇性失败。 */
            if (app_token_store_validate(a->src, tok)) {
                a->true_hits++;
            }
        }
        if ((i & 0x3ff) == 0) {
            app_token_store_clear(a->src); /* 周期性清槽,制造"空槽 vs 有值"切换 */
        }
    }
    return NULL;
}

static void t_concurrency(void)
{
    pthread_t  th[CONC_THREADS];
    conc_arg_t args[CONC_THREADS];
    char       probe[APP_TOKEN_HEX_LEN + 1];
    int        i;

    group("7. 并发冒烟(4 线程 x 20000 次)");

    test_clock_set_ms(3000000);
    for (i = 0; i < CONC_THREADS; i++) {
        /* 每两个线程共用一个来源:同源存在真实竞争,又保证"本线程刚签发"
         * 至少在另一线程不插手的窗口内有效。 */
        args[i].src       = (i % 2 == 0) ? TOKEN_SRC_BLE : TOKEN_SRC_SERIAL;
        args[i].true_hits = 0;
        args[i].issues    = 0;
    }
    for (i = 0; i < CONC_THREADS; i++) {
        pthread_create(&th[i], NULL, conc_worker, &args[i]);
    }
    for (i = 0; i < CONC_THREADS; i++) {
        pthread_join(th[i], NULL);
    }

    /* 死锁会让上面卡住;能走到这里就说明至少没锁死。
     * 断言口径:签发必须全部成功,且成功的签发里绝大多数能立刻通过校验
     * (同源并发会互相覆盖 + 周期 clear,所以不要求 100%)。 */
    int total_issues = 0;
    int total_hits   = 0;
    for (i = 0; i < CONC_THREADS; i++) {
        total_issues += args[i].issues;
        total_hits += args[i].true_hits;
    }
    CHECK_MSG(total_issues == CONC_THREADS * CONC_ITERS, "每一次 issue 都应成功");
    CHECK_MSG(total_hits > total_issues / 2,
              "刚签发的 token 应大概率立即可用(锁写漏会显著低于此)");

    /* 并发结束后状态仍可用 */
    CHECK(app_token_store_issue(TOKEN_SRC_BLE, probe, sizeof(probe)));
    CHECK(app_token_store_validate(TOKEN_SRC_BLE, probe));

    group_end();
}

/* ================================================================== main */
int main(void)
{
    printf("token store 单测 · auth_token.c (契约: APP_TOKEN_HEX_LEN=32, TTL=%dms)\n",
           APP_TOKEN_TTL_MS);

    t_issue_validate();
    t_reissue_invalidates_old();
    t_bad_args();
    t_format_check();
    t_expiry();
    t_clear_and_isolation();
    t_concurrency();

    printf("\n结果:%d 通过 / %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
