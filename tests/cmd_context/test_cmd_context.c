/* ============================================================================
 * cmd_context host 单测(C1 阶段 1,验收第 0 项)
 *
 * 只测 cmd_context.c 这一层,不经 cmd.c(分发器依赖 cJSON/wifi 等设备侧依赖,
 * 行为等价由 golden 契约测试在设备上验证)。钉死四条地基:
 *   1. UNKNOWN=0 零值默认拒绝;
 *   2. 入口构造不变量(source==origin/ENTRY/depth=0);
 *   3. 派生构造:origin 继承、depth+1、上限拒绝、trace 继承;
 *   4. INTERNAL 不作入口值。
 *
 * 编译:make -C tests/cmd_context
 * 运行:make -C tests/cmd_context run
 * ========================================================================== */
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>

#include "cmd_context.h"

static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond)                                                          \
    do {                                                                     \
        if (cond) {                                                          \
            g_pass++;                                                        \
        } else {                                                             \
            g_fail++;                                                        \
            printf("  FAIL  %s:%d  %s\n", __FILE__, __LINE__, #cond);        \
        }                                                                    \
    } while (0)

static void t_unknown_zero(void)
{
    CHECK(APP_CMD_SOURCE_UNKNOWN == 0);   /* 零值默认拒绝的物理基础 */
    CHECK(APP_CMD_SOURCE_MQTT != 0 && APP_CMD_SOURCE_BLE != 0);
}

static void t_entry_invariants(void)
{
    static const app_cmd_source_t entries[] = {
        APP_CMD_SOURCE_MQTT, APP_CMD_SOURCE_BLE, APP_CMD_SOURCE_HTTP,
        APP_CMD_SOURCE_TOUCH, APP_CMD_SOURCE_SERIAL,
    };
    size_t i;

    for (i = 0; i < sizeof(entries) / sizeof(entries[0]); i++) {
        app_cmd_context_t ctx = app_cmd_ctx_entry(entries[i]);

        CHECK(app_cmd_ctx_valid(&ctx));
        CHECK(ctx.source == entries[i]);
        CHECK(ctx.origin == entries[i]);          /* 入口:source==origin */
        CHECK(ctx.exec_env == APP_CMD_EXEC_ENTRY);
        CHECK(ctx.depth == 0);
        CHECK(ctx.trace_id != 0);                 /* 日志关联计数已填 */
        CHECK(ctx.channel_epoch == 0);            /* 阶段 1 占位 */
        CHECK(ctx.auth_level == APP_CMD_AUTH_NONE);
    }
}

static void t_invalid_entries(void)
{
    app_cmd_context_t ctx;

    ctx = app_cmd_ctx_entry(APP_CMD_SOURCE_UNKNOWN);
    CHECK(!app_cmd_ctx_valid(&ctx));              /* UNKNOWN 不可信 */

    ctx = app_cmd_ctx_entry(APP_CMD_SOURCE_INTERNAL);
    CHECK(!app_cmd_ctx_valid(&ctx));              /* INTERNAL 不作入口值 */

    CHECK(!app_cmd_ctx_valid(NULL));
}

static void t_internal_child(void)
{
    app_cmd_context_t parent = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);
    app_cmd_context_t child = {0};
    app_cmd_context_t grandchild = {0};
    app_cmd_context_t great = {0};

    CHECK(app_cmd_ctx_internal_child(&parent, &child));
    CHECK(app_cmd_ctx_valid(&child));
    CHECK(child.source == APP_CMD_SOURCE_INTERNAL);
    CHECK(child.origin == APP_CMD_SOURCE_BLE);    /* origin 继承,不得改写 */
    CHECK(child.exec_env == APP_CMD_EXEC_INTERNAL);
    CHECK(child.depth == 1);
    CHECK(child.trace_id == parent.trace_id);     /* 链路关联继承 */
    CHECK(child.channel_epoch == parent.channel_epoch);

    /* 深度上限:entry(0)→1→2 合法,再派生拒绝 */
    CHECK(app_cmd_ctx_internal_child(&child, &grandchild));
    CHECK(grandchild.depth == 2);
    CHECK(grandchild.origin == APP_CMD_SOURCE_BLE);
    CHECK(!app_cmd_ctx_internal_child(&grandchild, &great));

    /* 非法父上下文不得派生 */
    {
        app_cmd_context_t bad = app_cmd_ctx_entry(APP_CMD_SOURCE_UNKNOWN);

        CHECK(!app_cmd_ctx_internal_child(&bad, &great));
    }
    CHECK(!app_cmd_ctx_internal_child(&parent, NULL));
}

static void t_trace_increments(void)
{
    app_cmd_context_t a = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);
    app_cmd_context_t b = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);

    CHECK(b.trace_id != a.trace_id);              /* 每次入口请求独立 */
}

/* ---- 评审 R1-P1:校验收紧(非法枚举值/内部环境冒充/占位字段越界) ---- */
static void t_strict_validation(void)
{
    static const app_cmd_source_t wired[] = {
        APP_CMD_SOURCE_MQTT, APP_CMD_SOURCE_TOUCH, APP_CMD_SOURCE_SERIAL,
    };
    app_cmd_context_t child = {0};
    app_cmd_context_t parent;
    size_t i;

    /* 未定义入口值 99:入口构造即不可信 */
    parent = app_cmd_ctx_entry((app_cmd_source_t)99);
    CHECK(!app_cmd_ctx_valid(&parent));

    parent = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);
    CHECK(app_cmd_ctx_internal_child(&parent, &child));

    child.source = (app_cmd_source_t)99;          /* INTERNAL 环境伪造入口值 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.source = APP_CMD_SOURCE_BLE;            /* INTERNAL 环境冒充入口来源 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.source = APP_CMD_SOURCE_INTERNAL;       /* 恢复合法 */

    child.origin = (app_cmd_source_t)99;          /* origin 必须属于入口集合 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.origin = APP_CMD_SOURCE_INTERNAL;       /* INTERNAL 本身不是入口 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.origin = APP_CMD_SOURCE_BLE;

    child.exec_env = (app_cmd_exec_env_t)99;      /* exec_env 非法值整包拒绝 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.exec_env = APP_CMD_EXEC_INTERNAL;

    child.auth_level = (app_cmd_auth_level_t)99;  /* 占位字段越界同样拒绝 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.auth_level = APP_CMD_AUTH_LOCAL_TOKEN;  /* 合法占位值放行(仍无消费者) */
    CHECK(app_cmd_ctx_valid(&child));
    child.auth_level = APP_CMD_AUTH_NONE;

    child.channel_epoch = 1;                      /* epoch 恒 0 的占位契约 */
    CHECK(!app_cmd_ctx_valid(&child));
    child.channel_epoch = 0;
    CHECK(app_cmd_ctx_valid(&child));             /* 字段恢复后重新合法 */

    /* 已接线入口(MQTT/TOUCH)与预留入口(SERIAL)的 child 都合法 */
    for (i = 0; i < sizeof(wired) / sizeof(wired[0]); i++) {
        app_cmd_context_t c = {0};

        parent = app_cmd_ctx_entry(wired[i]);
        CHECK(app_cmd_ctx_valid(&parent));
        CHECK(app_cmd_ctx_internal_child(&parent, &c));
        CHECK(app_cmd_ctx_valid(&c));
    }
}

/* ---- 评审 R1-P2:trace 并发自增唯一性(4 线程 × 500 次 = 2000 个全不重) ---- */
#define T_THREADS 4
#define T_PER     500
static app_cmd_context_t g_trace_ctx[T_THREADS * T_PER];

static void *trace_worker(void *arg)
{
    long t = (long)arg;
    int i;

    for (i = 0; i < T_PER; i++) {
        g_trace_ctx[t * T_PER + i] = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);
    }
    return NULL;
}

static int trace_cmp(const void *a, const void *b)
{
    uint32_t x = ((const app_cmd_context_t *)a)->trace_id;
    uint32_t y = ((const app_cmd_context_t *)b)->trace_id;

    return (x > y) - (x < y);
}

static void t_trace_concurrent(void)
{
    pthread_t th[T_THREADS];
    size_t i;
    long t;

    for (t = 0; t < T_THREADS; t++) {
        CHECK(pthread_create(&th[t], NULL, trace_worker, (void *)t) == 0);
    }
    for (t = 0; t < T_THREADS; t++) {
        pthread_join(th[t], NULL);
    }
    qsort(g_trace_ctx, T_THREADS * T_PER, sizeof(g_trace_ctx[0]), trace_cmp);
    for (i = 1; i < T_THREADS * T_PER; i++) {
        if (g_trace_ctx[i].trace_id == g_trace_ctx[i - 1].trace_id) {
            break;
        }
    }
    CHECK(i == T_THREADS * T_PER);   /* 无任何重号 */
}

int main(void)
{
    printf("cmd_context 单测 · cmd_context.c (DEPTH_MAX=%d)\n", APP_CMD_CTX_DEPTH_MAX);

    t_unknown_zero();
    t_entry_invariants();
    t_invalid_entries();
    t_internal_child();
    t_trace_increments();
    t_strict_validation();
    t_trace_concurrent();

    printf("\n结果:%d 通过 / %d 失败\n", g_pass, g_fail);
    return g_fail == 0 ? 0 : 1;
}
