/*
 * CMD-03 Phase A · operation 层窄窗口 host 测试（S1–S4 / A1–A5 + A6 稳定性）
 *
 * 目标：证明 cancel/expire 置 expiring 后，cleanup 已返回、finalize 之前的窗口里，
 *       `app_operation_set_state(GOT_IP)` 必被拒绝，终态唯一，不出现"复活"。
 *
 * **边界**：本测试只覆盖 operation 层。它 **不能** 证明 cmd.c 的
 * `app_cmd_net_store_trial()` 不会在 cleanup 之后重新写入 s_net_trial —— 那属于 Phase B。
 *
 * 每个 scenario 在 fork() 子进程中运行（operation 的静态表与常驻线程无法优雅销毁），
 * 父进程通过管道收集子进程的逐项断言结果。
 */
#define _GNU_SOURCE
#include <errno.h>
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <unistd.h>

#include "operation.h"
#include "operation_state.h"
#include "rtos_shim.h"

#define REPS 50
#define MAX_ASSERTS 8

/* ------------------------------------------------------------------ */
/* fake run/cleanup                                                    */
/* ------------------------------------------------------------------ */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    int cleanup_done;
    int cleanup_fail;
    int mode;              /* 0=wait cleanup then OK; 1=set GOT_IP; 2=return OK */
} ctx_t;

enum { MODE_WAIT = 0, MODE_GOT_IP = 1, MODE_RETURN_OK = 2 };

static void ctx_init(ctx_t *x, int mode, int cleanup_fail)
{
    memset(x, 0, sizeof(*x));
    pthread_mutex_init(&x->m, NULL);
    pthread_cond_init(&x->c, NULL);
    x->mode = mode;
    x->cleanup_fail = cleanup_fail;
}

static esp_err_t fake_run(app_operation_t *op, void *v)
{
    ctx_t *x = (ctx_t *)v;

    if (x->mode == MODE_GOT_IP) {
        (void)app_operation_set_state(op, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE);
        return ESP_OK;
    }
    if (x->mode == MODE_RETURN_OK) {
        return ESP_OK;
    }
    /* MODE_WAIT：等 cleanup 发生后再返回 OK —— 制造"cleanup 先、run 后"的时序 */
    pthread_mutex_lock(&x->m);
    while (!x->cleanup_done) {
        pthread_cond_wait(&x->c, &x->m);
    }
    pthread_mutex_unlock(&x->m);
    return ESP_OK;
}

static bool fake_cleanup(app_operation_t *op, void *v)
{
    ctx_t *x = (ctx_t *)v;

    (void)op;
    pthread_mutex_lock(&x->m);
    x->cleanup_done = 1;
    pthread_cond_broadcast(&x->c);
    pthread_mutex_unlock(&x->m);
    return x->cleanup_fail ? false : true;
}

/* ------------------------------------------------------------------ */
/* 断言与等待                                                          */
/* ------------------------------------------------------------------ */
static int g_all_ok;

static void report(const char *id, int cond)
{
    printf("ASSERT %s=%s\n", id, cond ? "PASS" : "FAIL");
    if (!cond) {
        g_all_ok = 0;
    }
}

static bool wait_state(app_operation_t *op, app_op_state_t want, int timeout_ms)
{
    int loops = timeout_ms / 5;

    for (int i = 0; i < loops; i++) {
        app_operation_status_t st;

        if (app_operation_get_status(op, &st) && st.state == want) {
            return true;
        }
        usleep(5000);
    }
    return false;
}

/* 时钟驱动线程：确认 hook 已进入后推进虚拟时间，直到 hook 退出。
 * 不能由调用 cancel() 的线程推进——那会在 hook 内自锁。 */
static void *clock_driver(void *arg)
{
    (void)arg;

    for (int i = 0; i < 5000; i++) {
        if (op_test_hook_entered()) {
            break;
        }
        usleep(1000);
    }
    if (!op_test_hook_entered()) {
        return NULL;
    }
    for (int i = 0; i < 400 && !op_test_hook_exited(); i++) {
        op_test_advance_us(50 * 1000);
        usleep(1000);
    }
    return NULL;
}

static bool probe_rejected(void)
{
    const char *p = op_test_hook_probe_log();

    return p != NULL && strstr(p, "rejected by expiring gate") != NULL;
}

/* ------------------------------------------------------------------ */
/* S1 · cancel 窗口                                                    */
/* ------------------------------------------------------------------ */
static int scenario_s1(void)
{
    ctx_t x;
    app_operation_t *op = NULL;
    app_operation_t *op2 = NULL;
    app_operation_status_t st;
    pthread_t drv;

    app_operation_init();
    ctx_init(&x, MODE_WAIT, 0);
    if (app_operation_start(APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
                            fake_run, fake_cleanup, &x, &op) != ESP_OK) {
        report("A2", 0);
        return 1;
    }
    (void)wait_state(op, APP_OP_CONNECTING, 2000);
    app_operation_test_hook_arm(1000, APP_OP_GOT_IP);
    pthread_create(&drv, NULL, clock_driver, NULL);
    esp_err_t cr = app_operation_cancel(op);
    pthread_join(drv, NULL);

    report("A1", probe_rejected());
    report("A2", cr == ESP_OK && app_operation_get_status(op, &st) &&
                 st.state == APP_OP_CANCELLED);
    /* A3：新 op 可受理（不声称 get_status 能看到 expiring） */
    {
        ctx_t y;
        ctx_init(&y, MODE_RETURN_OK, 0);
        report("A3", app_operation_start(APP_OPERATION_OWNER_BLE, NULL, 20000,
                                         300000, fake_run, fake_cleanup,
                                         &y, &op2) == ESP_OK);
    }
    return g_all_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* S2 · expire 窗口                                                    */
/* ------------------------------------------------------------------ */
static int scenario_s2(void)
{
    ctx_t x;
    app_operation_t *op = NULL;
    app_operation_t *op2 = NULL;
    app_operation_status_t st;
    pthread_t drv;

    app_operation_init();
    ctx_init(&x, MODE_WAIT, 0);
    if (app_operation_start(APP_OPERATION_OWNER_MQTT, NULL, 200, 300000,
                            fake_run, fake_cleanup, &x, &op) != ESP_OK) {
        report("A2", 0);
        return 1;
    }
    (void)wait_state(op, APP_OP_CONNECTING, 2000);
    app_operation_test_hook_arm(1000, APP_OP_GOT_IP);
    pthread_create(&drv, NULL, clock_driver, NULL);
    /* 越过 accepted TTL（虚拟时间），让 expire 任务触发 cleanup 并进入 hook */
    op_test_advance_us(5000 * 1000);
    pthread_join(drv, NULL);
    (void)wait_state(op, APP_OP_EXPIRED, 3000);

    report("A1", probe_rejected());
    report("A2", app_operation_get_status(op, &st) && st.state == APP_OP_EXPIRED);
    {
        ctx_t y;
        ctx_init(&y, MODE_RETURN_OK, 0);
        report("A3", app_operation_start(APP_OPERATION_OWNER_BLE, NULL, 20000,
                                         300000, fake_run, fake_cleanup,
                                         &y, &op2) == ESP_OK);
    }
    return g_all_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* S3 · 正常成功（防守卫过严）                                          */
/* ------------------------------------------------------------------ */
static int scenario_s3(void)
{
    ctx_t x;
    app_operation_t *op = NULL;
    app_operation_status_t st;

    app_operation_init();
    ctx_init(&x, MODE_GOT_IP, 0);
    if (app_operation_start(APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
                            fake_run, fake_cleanup, &x, &op) != ESP_OK) {
        report("A4", 0);
        return 1;
    }
    report("A4", wait_state(op, APP_OP_GOT_IP, 3000) &&
                 app_operation_get_status(op, &st) && st.state == APP_OP_GOT_IP);
    return g_all_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* S4 · cleanup 失败（无 post_cleanup hook，不参与 A1）                 */
/* ------------------------------------------------------------------ */
static int scenario_s4(void)
{
    ctx_t x;
    app_operation_t *op = NULL;
    app_operation_status_t st;

    app_operation_init();
    ctx_init(&x, MODE_RETURN_OK, 1);
    if (app_operation_start(APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
                            fake_run, fake_cleanup, &x, &op) != ESP_OK) {
        report("A5", 0);
        return 1;
    }
    (void)wait_state(op, APP_OP_CONNECTING, 2000);
    (void)app_operation_cancel(op);
    report("A5", app_operation_get_status(op, &st) &&
                 st.state == APP_OP_FAILED &&
                 st.error == APP_OPERATION_ERR_CLEANUP_FAILED);
    /* S4 不应触发 hook（cleanup 失败直接返回），顺带断言 */
    report("A5h", !op_test_hook_entered());
    return g_all_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* S5 · 负向对照：同一 set_state 在**非** expiring 时必须成功            */
/* 用于证明 A1 的拒绝来自 expiring 闸，而不是探针永远失败。               */
/* ------------------------------------------------------------------ */
static int scenario_s5(void)
{
    ctx_t x;
    app_operation_t *op = NULL;
    app_operation_status_t st;

    app_operation_init();
    ctx_init(&x, MODE_RETURN_OK, 0);
    if (app_operation_start(APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
                            fake_run, fake_cleanup, &x, &op) != ESP_OK) {
        report("A7", 0);
        return 1;
    }
    (void)wait_state(op, APP_OP_CONNECTING, 2000);
    esp_err_t e = app_operation_set_state(op, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE);
    report("A7", e == ESP_OK && app_operation_get_status(op, &st) &&
                 st.state == APP_OP_GOT_IP);
    return g_all_ok ? 0 : 1;
}

/* ------------------------------------------------------------------ */
/* fork 运行器：按断言号统计                                            */
/* ------------------------------------------------------------------ */
typedef struct {
    char id[16];
    int pass;
    int total;
} talley_t;

static talley_t g_tally[MAX_ASSERTS];
static int g_tally_n;

static void tally(const char *id, int ok)
{
    for (int i = 0; i < g_tally_n; i++) {
        if (strcmp(g_tally[i].id, id) == 0) {
            g_tally[i].total++;
            g_tally[i].pass += ok;
            return;
        }
    }
    if (g_tally_n < MAX_ASSERTS) {
        snprintf(g_tally[g_tally_n].id, sizeof(g_tally[g_tally_n].id), "%s", id);
        g_tally[g_tally_n].total = 1;
        g_tally[g_tally_n].pass = ok;
        g_tally_n++;
    }
}

static void parse_child_output(const char *buf)
{
    const char *p = buf;

    while ((p = strstr(p, "ASSERT ")) != NULL) {
        char id[16];
        char verdict[8];
        p += 7;
        if (sscanf(p, "%15[^=]=%7s", id, verdict) == 2) {
            tally(id, strncmp(verdict, "PASS", 4) == 0);
        }
        p = strchr(p, '\n') ? strchr(p, '\n') + 1 : p + strlen(p);
    }
}

static int run_scenario(const char *name, int (*fn)(void), int reps)
{
    int iter_ok = 0;

    for (int i = 0; i < reps; i++) {
        int fd[2];
        pid_t pid;
        char buf[4096] = {0};
        ssize_t n;
        int status;

        if (pipe(fd) != 0) {
            printf("SCN %s pipe failed\n", name);
            return 0;
        }
        pid = fork();
        if (pid == 0) {
            close(fd[0]);
            dup2(fd[1], STDOUT_FILENO);
            close(fd[1]);
            g_all_ok = 1;
            int rc = fn();
            fflush(stdout);
            _exit(rc);
        }
        close(fd[1]);
        n = read(fd[0], buf, sizeof(buf) - 1);
        if (n > 0) {
            buf[n] = '\0';
        }
        close(fd[0]);
        (void)waitpid(pid, &status, 0);
        parse_child_output(buf);
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            iter_ok++;
        } else {
            printf("SCN %s iter %d FAILED (status=0x%x)\n", name, i, status);
            printf("%s", buf);
        }
    }
    printf("SCN %s: %d/%d iterations passed\n", name, iter_ok, reps);
    return iter_ok == reps;
}

int main(int argc, char **argv)
{
    int verbose = 0;
    int all;
    int reps = REPS;
    const char *env = getenv("OPS_REPS");
    if (env != NULL && atoi(env) > 0) {
        reps = atoi(env);
    }

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-v") == 0) {
            verbose = 1;
        }
    }
    op_test_set_verbose(verbose);

    all = run_scenario("S1_cancel", scenario_s1, reps);
    all &= run_scenario("S2_expire", scenario_s2, reps);
    all &= run_scenario("S3_success", scenario_s3, reps);
    all &= run_scenario("S4_cleanup_fail", scenario_s4, reps);
    all &= run_scenario("S5_negative_control", scenario_s5, reps);

    printf("\n=== 逐项断言汇总 ===\n");
    for (int i = 0; i < g_tally_n; i++) {
        printf("  %-3s %d/%d\n", g_tally[i].id, g_tally[i].pass, g_tally[i].total);
        if (g_tally[i].pass != g_tally[i].total) {
            all = 0;
        }
    }
    printf("\nRESULT: %s\n", all ? "PASS" : "FAIL");
    return all ? 0 : 1;
}
