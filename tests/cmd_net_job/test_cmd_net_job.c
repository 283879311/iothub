#define _GNU_SOURCE
#include <pthread.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#include "cmd_context.h"
#include "cmd_net_job.h"
#include "job_port_shim.h"
#include "operation.h"
#include "operation_state.h"
#include "rtos_shim.h"

/* ------------------------------------------------------------------ */
/* run context / operation helpers                                     */
/* ------------------------------------------------------------------ */
typedef struct {
    pthread_mutex_t m;
    pthread_cond_t c;
    bool entered;
    bool release;
    uint32_t gen;
} run_ctx_t;

/* cmd_net_job 把 run_fn 的 ctx 固定为内部 &s_job，不能当作 run_ctx_t 使用；
 * 单 worker 串行执行，run_fn 通过本指针取当前用例的同步上下文。 */
static run_ctx_t *g_run_ctx;

static esp_err_t test_run(app_operation_t *op, void *v)
{
    run_ctx_t *r = g_run_ctx;
    app_operation_status_t st;

    (void)v;
    if (r == NULL) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!app_operation_get_status(op, &st)) {
        return ESP_ERR_INVALID_ARG;
    }
    pthread_mutex_lock(&r->m);
    r->gen = st.generation;
    r->entered = true;
    pthread_cond_broadcast(&r->c);
    while (!r->release) {
        pthread_cond_wait(&r->c, &r->m);
    }
    pthread_mutex_unlock(&r->m);
    return ESP_OK;
}

static void run_ctx_init(run_ctx_t *r)
{
    memset(r, 0, sizeof(*r));
    pthread_mutex_init(&r->m, NULL);
    pthread_cond_init(&r->c, NULL);
}

static bool start_op(run_ctx_t *r, app_operation_t **out, uint32_t ttl_ms,
                     const char *ssid)
{    app_cmd_context_t ctx = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);
    app_operation_t *op = NULL;
    bool ok = false;

    run_ctx_init(r);
    g_run_ctx = r;
    app_cmd_net_job_lock();
    if (app_cmd_net_job_begin_locked(APP_CMD_SOURCE_BLE, APP_OPERATION_OWNER_BLE,
                                     &ctx, ssid, "pass", NULL, ttl_ms, 300000,
                                     test_run, &op) == APP_CMD_NET_JOB_BEGIN_OK) {
        ok = true;
    }
    app_cmd_net_job_unlock();
    if (!ok) {
        return false;
    }
    pthread_mutex_lock(&r->m);
    for (int i = 0; i < 400 && !r->entered; i++) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) {
            ts.tv_sec++;
            ts.tv_nsec -= 1000000000L;
        }
        pthread_cond_timedwait(&r->c, &r->m, &ts);
    }
    ok = r->entered;
    pthread_mutex_unlock(&r->m);
    *out = op;
    return ok;
}

/* 只建立 job/operation,不等待 worker 运行。用于 A 的 run_fn 占住唯一
 * operation worker 时创建 B/E;此时 B/E 为 ACCEPTED,job 归属仍可验证。 */
static bool begin_job_only(run_ctx_t *r, app_operation_t **out, uint32_t ttl_ms,
                           const char *ssid)
{
    app_cmd_context_t ctx = app_cmd_ctx_entry(APP_CMD_SOURCE_BLE);
    app_operation_t *op = NULL;
    bool ok = false;

    run_ctx_init(r);
    g_run_ctx = r;
    app_cmd_net_job_lock();
    if (app_cmd_net_job_begin_locked(APP_CMD_SOURCE_BLE, APP_OPERATION_OWNER_BLE,
                                     &ctx, ssid, "pass", NULL, ttl_ms, 300000,
                                     test_run, &op) == APP_CMD_NET_JOB_BEGIN_OK) {
        ok = true;
    }
    app_cmd_net_job_unlock();
    *out = op;
    return ok;
}

static bool wait_op_state(app_operation_t *op, app_op_state_t want, int timeout_ms)
{
    for (int i = 0; i < timeout_ms / 5; i++) {
        app_operation_status_t st;
        if (app_operation_get_status(op, &st) && st.state == want) {
            return true;
        }
        usleep(5000);
    }
    return false;
}

static app_cmd_net_job_snap_t job_snapshot(void)
{
    app_cmd_net_job_snap_t s;
    app_cmd_net_job_lock();
    app_cmd_net_job_snapshot_locked(&s);
    app_cmd_net_job_unlock();
    return s;
}

static app_cmd_net_trial_snap_t trial_snapshot(void)
{
    app_cmd_net_trial_snap_t s;
    app_cmd_net_job_lock();
    app_cmd_net_trial_snapshot_locked(&s);
    app_cmd_net_job_unlock();
    return s;
}

static app_operation_cleanup_state_t claim_state(app_operation_t *op, uint32_t gen)
{
    return app_operation_cleanup_result(op, gen);
}

/* ------------------------------------------------------------------ */
/* hook controller                                                     */
/* ------------------------------------------------------------------ */
static pthread_mutex_t g_hook_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_hook_c = PTHREAD_COND_INITIALIZER;
static const char *g_wait_point;
static bool g_hook_reached;
static bool g_hook_release;
static int g_before_state = -1;

static void hook_fn(const char *point, app_operation_t *op, uint32_t gen)
{
    (void)gen;
    if (strcmp(point, "cleanup_before_complete") == 0 && op != NULL) {
        app_operation_status_t st;
        if (app_operation_get_status(op, &st)) {
            g_before_state = (int)st.state;
        }
    }
    pthread_mutex_lock(&g_hook_m);
    if (g_wait_point != NULL && strcmp(point, g_wait_point) == 0) {
        g_hook_reached = true;
        pthread_cond_broadcast(&g_hook_c);
        while (!g_hook_release) {
            pthread_cond_wait(&g_hook_c, &g_hook_m);
        }
    }
    pthread_mutex_unlock(&g_hook_m);
}

static void hook_wait_for(const char *point)
{
    pthread_mutex_lock(&g_hook_m);
    g_wait_point = point;
    g_hook_reached = false;
    g_hook_release = false;
    pthread_mutex_unlock(&g_hook_m);
}

static bool hook_wait_reached(int timeout_ms)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    ts.tv_sec += timeout_ms / 1000;
    ts.tv_nsec += (long)(timeout_ms % 1000) * 1000000L;
    if (ts.tv_nsec >= 1000000000L) {
        ts.tv_sec++;
        ts.tv_nsec -= 1000000000L;
    }
    pthread_mutex_lock(&g_hook_m);
    while (!g_hook_reached) {
        if (pthread_cond_timedwait(&g_hook_c, &g_hook_m, &ts) != 0) {
            break;
        }
    }
    bool ok = g_hook_reached;
    pthread_mutex_unlock(&g_hook_m);
    return ok;
}

static void hook_release(void)
{
    pthread_mutex_lock(&g_hook_m);
    g_hook_release = true;
    pthread_cond_broadcast(&g_hook_c);
    pthread_mutex_unlock(&g_hook_m);
}

/* ------------------------------------------------------------------ */
/* worker threads                                                      */
/* ------------------------------------------------------------------ */
typedef struct {
    app_operation_t *op;
    uint32_t gen;
    app_cmd_net_trial_store_result_t store_result;
    bool owned_result;
} job_arg_t;

static void *trial_store_thread(void *p)
{
    job_arg_t *a = (job_arg_t *)p;
    a->store_result = app_cmd_net_trial_store("A", "pass", APP_CMD_SOURCE_BLE,
                                              "op-A", NULL, a->op, NULL);
    return NULL;
}

static void *netcheck_thread(void *p)
{
    job_arg_t *a = (job_arg_t *)p;
    a->owned_result = app_cmd_net_job_set_netcheck_gen(a->op, a->gen, 0xABC);
    return NULL;
}

static void *stages_thread(void *p)
{
    job_arg_t *a = (job_arg_t *)p;
    a->owned_result = app_cmd_net_job_set_stages(a->op, a->gen, true, true, true, true);
    return NULL;
}

static void *clear_thread(void *p)
{
    job_arg_t *a = (job_arg_t *)p;
    a->owned_result = app_cmd_net_job_clear_if_owned(a->op, a->gen);
    return NULL;
}

typedef struct {
    app_operation_t *op;
    bool result;
} cleanup_arg_t;

static void *cleanup_thread(void *p)
{
    cleanup_arg_t *a = (cleanup_arg_t *)p;
    a->result = app_cmd_net_cleanup_run(a->op, NULL);
    return NULL;
}

typedef struct {
    app_operation_t *op;
    uint32_t gen;
    app_op_state_t target_state;
    app_operation_error_t target_error;
    app_operation_finish_result_t result;
} finish_arg_t;

static void *finish_thread(void *p)
{
    finish_arg_t *a = (finish_arg_t *)p;
    a->result = app_operation_request_finish(
        a->op, a->gen, a->target_state, a->target_error,
        APP_OPERATION_CLEANUP_REQUIRED);
    return NULL;
}

/* ------------------------------------------------------------------ */
/* init / ticker                                                       */
/* ------------------------------------------------------------------ */
static void *ticker(void *arg)
{
    (void)arg;
    for (;;) {
        op_test_advance_us(1000);
        usleep(1000);
    }
    return NULL;
}

static void init_all(void)
{
    pthread_t t;

    job_test_port_reset();
    op_test_reset_defer_counts();
    job_test_port_install();
    if (app_operation_init() != ESP_OK) {
        fprintf(stderr, "operation init failed\n");
        _exit(2);
    }
    app_cmd_net_job_test_hook_set(hook_fn);
    pthread_create(&t, NULL, ticker, NULL);
    pthread_detach(t);
}

/* ------------------------------------------------------------------ */
/* cases                                                               */
/* ------------------------------------------------------------------ */
static bool t01(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    job_arg_t arg = {0};
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    arg.op = A;
    hook_wait_for("trial_before_lock");
    pthread_create(&th, NULL, trial_store_thread, &arg);
    bool reached = hook_wait_reached(2000);
    (void)app_operation_cancel(A);
    hook_release();
    pthread_join(th, NULL);
    app_cmd_net_trial_snap_t ts = trial_snapshot();

    return reached && arg.store_result == APP_CMD_NET_TRIAL_STALE_JOB && !ts.valid;
}

static bool t02(void)
{
    run_ctx_t a;
    run_ctx_t b;
    app_operation_t *A = NULL;
    app_operation_t *B = NULL;
    job_arg_t arg = {0};
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    arg.op = A;
    hook_wait_for("trial_before_lock");
    pthread_create(&th, NULL, trial_store_thread, &arg);
    bool reached = hook_wait_reached(2000);
    (void)app_operation_cancel(A);
    if (!begin_job_only(&b, &B, 30000, "B")) {
        hook_release();
        pthread_join(th, NULL);
        return false;
    }
    hook_release();
    pthread_join(th, NULL);
    app_cmd_net_job_snap_t s = job_snapshot();

    return reached && arg.store_result == APP_CMD_NET_TRIAL_STALE_OP &&
           s.active && strcmp(s.operation_id, app_operation_id(B)) == 0;
}

static bool t03(void)
{
    run_ctx_t a;
    run_ctx_t b;
    app_operation_t *A = NULL;
    app_operation_t *B = NULL;
    job_arg_t arg = {0};
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    arg.op = A;
    arg.gen = a.gen;
    hook_wait_for("netcheck_before_lock");
    pthread_create(&th, NULL, netcheck_thread, &arg);
    bool reached = hook_wait_reached(2000);
    (void)app_operation_cancel(A);
    if (!begin_job_only(&b, &B, 30000, "B")) {
        hook_release();
        pthread_join(th, NULL);
        return false;
    }
    hook_release();
    pthread_join(th, NULL);
    app_cmd_net_job_snap_t s = job_snapshot();

    return reached && !arg.owned_result && s.active && s.netcheck_gen == 0;
}

static bool t04(void)
{
    run_ctx_t a;
    run_ctx_t b;
    app_operation_t *A = NULL;
    app_operation_t *B = NULL;
    job_arg_t arg = {0};
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    arg.op = A;
    arg.gen = a.gen;
    hook_wait_for("stages_before_lock");
    pthread_create(&th, NULL, stages_thread, &arg);
    bool reached = hook_wait_reached(2000);
    (void)app_operation_cancel(A);
    if (!begin_job_only(&b, &B, 30000, "B")) {
        hook_release();
        pthread_join(th, NULL);
        return false;
    }
    hook_release();
    pthread_join(th, NULL);
    app_cmd_net_job_snap_t s = job_snapshot();

    return reached && !arg.owned_result && s.active &&
           !s.st_wifi_connected && !s.st_ip_acquired &&
           !s.st_internet_verified && !s.st_mqtt_connected;
}

static bool t05(void)
{
    run_ctx_t a;
    run_ctx_t b;
    app_operation_t *A = NULL;
    app_operation_t *B = NULL;
    job_arg_t arg = {0};
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    arg.op = A;
    arg.gen = a.gen;
    hook_wait_for("clear_before_lock");
    pthread_create(&th, NULL, clear_thread, &arg);
    bool reached = hook_wait_reached(2000);
    (void)app_operation_cancel(A);
    if (!begin_job_only(&b, &B, 30000, "B")) {
        hook_release();
        pthread_join(th, NULL);
        return false;
    }
    hook_release();
    pthread_join(th, NULL);
    app_cmd_net_job_snap_t s = job_snapshot();

    return reached && !arg.owned_result && s.active &&
           strcmp(s.operation_id, app_operation_id(B)) == 0;
}

static bool t06(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c1 = {0};
    cleanup_arg_t c2 = {0};
    pthread_t th1;
    pthread_t th2;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    c1.op = A;
    c2.op = A;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th1, NULL, cleanup_thread, &c1);
    bool reached = hook_wait_reached(2000);
    pthread_create(&th2, NULL, cleanup_thread, &c2);
    usleep(50000);
    hook_release();
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    return reached && c1.result && c2.result &&
           job_test_rp_count() == 1 && job_test_nc_count() == 1 &&
           job_test_tc_count() == 1;
}

static bool t07(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c1 = {0};
    cleanup_arg_t c2 = {0};

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    job_test_set_cleanup_result(false);
    c1.op = A;
    c2.op = A;
    c1.result = app_cmd_net_cleanup_run(A, NULL);
    app_operation_cleanup_state_t st1 = claim_state(A, a.gen);
    c2.result = app_cmd_net_cleanup_run(A, NULL);

    return !c1.result && !c2.result && st1 == APP_OPERATION_CLEANUP_FAIL &&
           job_test_rp_count() == 1;
}

static bool t08(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c1 = {0};
    cleanup_arg_t c2 = {0};
    pthread_t th1;
    pthread_t th2;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    c1.op = A;
    c2.op = A;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th1, NULL, cleanup_thread, &c1);
    bool reached = hook_wait_reached(2000);
    pthread_create(&th2, NULL, cleanup_thread, &c2);
    usleep(50000);
    hook_release();
    pthread_join(th1, NULL);
    pthread_join(th2, NULL);

    return reached && c1.result && c2.result && job_test_rp_count() == 1;
}

static int64_t mono_ms(void)
{
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static bool t09_common(bool measure)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c1 = {0};
    pthread_t th1;
    int64_t t0;
    int64_t elapsed = 0;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    c1.op = A;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th1, NULL, cleanup_thread, &c1);
    if (!hook_wait_reached(2000)) {
        hook_release();
        pthread_join(th1, NULL);
        return false;
    }
    t0 = mono_ms();
    (void)app_operation_cancel(A);
    elapsed = mono_ms() - t0;

    app_operation_status_t st;
    bool got = app_operation_get_status(A, &st);
    bool state_ok = got && st.state == APP_OP_FAILED &&
                    st.error == APP_OPERATION_ERR_CLEANUP_FAILED;
    bool claim_running = claim_state(A, a.gen) == APP_OPERATION_CLEANUP_RUNNING;

    hook_release();
    pthread_join(th1, NULL);

    bool claim_ok = claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK;
    bool before_ok = g_before_state == (int)APP_OP_FAILED;
    bool rp_ok = job_test_rp_count() == 1;

    if (measure) {
        return state_ok && claim_running && claim_ok && before_ok && rp_ok &&
               elapsed >= 200 && elapsed <= 400;
    }
    return state_ok && claim_running && claim_ok && before_ok && rp_ok;
}

static bool g_expected;

static void mark_expected(void)
{
    g_expected = true;
}

static bool t09a(void)
{
    bool ok = t09_common(false);
    if (ok) {
        /* 语义不一致是冻结设计中的 EXPECTED 结果，不是普通 PASS。 */
        mark_expected();
    }
    return ok;
}

static bool t09b(void)
{
    return t09_common(true);
}

static bool t10(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c1 = {0};
    pthread_t th1;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    c1.op = A;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th1, NULL, cleanup_thread, &c1);
    if (!hook_wait_reached(2000)) {
        return false;
    }
    (void)app_operation_cancel(A);
    app_operation_status_t st;
    bool state_ok = app_operation_get_status(A, &st) && st.state == APP_OP_FAILED &&
                    st.error == APP_OPERATION_ERR_CLEANUP_FAILED;
    bool running = claim_state(A, a.gen) == APP_OPERATION_CLEANUP_RUNNING;
    app_cmd_net_job_lock();
    app_cmd_net_job_unlock();
    /* 第一方在 cleanup_request 之前保持阻塞：等待方有界退出且不得代跑 RP。
     * 子进程退出即回收仍在 hook 上的第一方线程。 */
    return state_ok && running && job_test_rp_count() == 0;
}

static bool t11(void)
{
    run_ctx_t a;
    run_ctx_t b;
    app_operation_t *A = NULL;
    app_operation_t *B = NULL;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    (void)app_operation_cancel(A);
    if (!wait_op_state(A, APP_OP_CANCELLED, 2000)) {
        return false;
    }
    app_operation_cleanup_state_t before = claim_state(A, a.gen);
    app_operation_cleanup_complete(A, a.gen + 1, true);
    app_operation_cleanup_state_t after_bad_gen = claim_state(A, a.gen);
    if (!begin_job_only(&b, &B, 30000, "B")) {
        return false;
    }
    app_operation_cleanup_complete(A, a.gen, true);
    app_cmd_net_job_snap_t s = job_snapshot();

    return before == APP_OPERATION_CLEANUP_OK &&
           after_bad_gen == APP_OPERATION_CLEANUP_OK &&
           s.active && strcmp(s.operation_id, app_operation_id(B)) == 0;
}

static bool t12(void)
{
    run_ctx_t a;
    run_ctx_t e;
    app_operation_t *A = NULL;
    app_operation_t *E = NULL;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    (void)app_operation_cancel(A);
    if (!wait_op_state(A, APP_OP_CANCELLED, 2000) || job_test_rp_count() != 1) {
        return false;
    }
    if (!begin_job_only(&e, &E, 50, "E")) {
        return false;
    }
    bool expired = wait_op_state(E, APP_OP_EXPIRED, 5000);

    return expired && job_test_rp_count() == 2 && job_test_nc_count() == 2;
}

static bool t13(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    finish_arg_t f = {0};
    pthread_t th;
    app_operation_status_t st;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    f.op = A;
    f.gen = a.gen;
    f.target_state = APP_OP_CANCELLED;
    f.target_error = APP_OPERATION_ERR_CANCELLED;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th, NULL, finish_thread, &f);
    bool reached = hook_wait_reached(2000);
    int begin_count = op_test_defer_begin_count();
    app_operation_finish_result_t second = app_operation_request_finish(
        A, a.gen, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    int ends_while_pending = op_test_defer_end_count();
    hook_release();
    pthread_join(th, NULL);
    bool got = app_operation_get_status(A, &st);

    return reached && second == APP_OPERATION_FINISH_PENDING &&
           f.result == APP_OPERATION_FINISH_DONE && got &&
           st.state == APP_OP_CANCELLED &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_rp_count() == 1 && begin_count == 1 &&
           ends_while_pending == 0 && op_test_defer_end_count() == 1;
}

static bool t14(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    app_operation_status_t st;
    app_operation_finish_result_t finish;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    if (app_operation_set_state(A, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE) != ESP_OK ||
        app_operation_set_state(A, APP_OP_COMMITTING, APP_OPERATION_ERR_NONE) != ESP_OK) {
        return false;
    }
    int ends_before = op_test_defer_end_count();
    finish = app_operation_request_finish(
        A, a.gen, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    bool got = app_operation_get_status(A, &st);
    int ends_after_reject = op_test_defer_end_count();
    bool success = app_operation_set_state(A, APP_OP_SUCCESS,
                                          APP_OPERATION_ERR_NONE) == ESP_OK;

    return finish == APP_OPERATION_FINISH_REJECTED && got &&
           st.state == APP_OP_COMMITTING && ends_before == 0 &&
           ends_after_reject == 0 && success &&
           op_test_defer_end_count() == 1;
}

static bool t15(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    app_cmd_net_release_result_t rel = app_cmd_net_release_if_owned(A, a.gen);
    app_cmd_net_job_snap_t s = job_snapshot();

    return rel == APP_CMD_NET_RELEASE_DONE && !s.active &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_clear_count() >= 1;
}

static bool t16(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    char op_id[APP_OPERATION_ID_LEN];

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    snprintf(op_id, sizeof(op_id), "%s", app_operation_id(A));
    if (app_cmd_net_trial_store("A", "pass", APP_CMD_SOURCE_BLE,
                                op_id, NULL, A, NULL) != APP_CMD_NET_TRIAL_STORED) {
        return false;
    }
    app_cmd_net_job_lock();
    app_cmd_net_job_clear_locked();
    app_cmd_net_job_unlock();

    app_cmd_net_release_result_t rel = app_cmd_net_release_if_owned(A, a.gen);
    app_cmd_net_trial_snap_t t = trial_snapshot();

    return rel == APP_CMD_NET_RELEASE_DONE && !t.valid &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK;
}

static bool t17(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c = {0};
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    c.op = A;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th, NULL, cleanup_thread, &c);
    if (!hook_wait_reached(2000)) {
        hook_release();
        pthread_join(th, NULL);
        return false;
    }
    app_cmd_net_release_result_t rel = app_cmd_net_release_if_owned(A, a.gen);
    app_cmd_net_job_snap_t s = job_snapshot();
    app_cmd_net_trial_snap_t t = trial_snapshot();
    app_operation_cleanup_state_t running = claim_state(A, a.gen);
    int rp_while_blocked = job_test_rp_count();
    hook_release();
    pthread_join(th, NULL);

    return rel == APP_CMD_NET_RELEASE_PENDING && !s.active && !t.valid &&
           running == APP_OPERATION_CLEANUP_RUNNING && rp_while_blocked == 0 &&
           c.result && claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_rp_count() == 1;
}

static bool t18(void)
{
    run_ctx_t a;
    run_ctx_t b;
    app_operation_t *A = NULL;
    app_operation_t *B = NULL;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    (void)app_operation_cancel(A);
    if (!begin_job_only(&b, &B, 30000, "B")) {
        return false;
    }
    app_cmd_net_release_result_t rel = app_cmd_net_release_if_owned(A, a.gen);
    app_cmd_net_job_snap_t s = job_snapshot();

    return rel == APP_CMD_NET_RELEASE_STALE && s.active &&
           strcmp(s.operation_id, app_operation_id(B)) == 0;
}

static bool t19(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    char op_id[APP_OPERATION_ID_LEN];
    app_operation_status_t st;
    app_cmd_net_trial_snap_t t;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    snprintf(op_id, sizeof(op_id), "%s", app_operation_id(A));
    if (app_cmd_net_trial_store("A", "pass", APP_CMD_SOURCE_BLE,
                                op_id, NULL, A, NULL) != APP_CMD_NET_TRIAL_STORED) {
        return false;
    }
    app_cmd_net_job_lock();
    app_cmd_net_job_clear_locked();
    app_cmd_net_job_unlock();

    app_operation_finish_result_t finish = app_operation_request_finish(
        A, a.gen, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    bool got = app_operation_get_status(A, &st);
    t = trial_snapshot();

    return finish == APP_OPERATION_FINISH_DONE && got &&
           st.state == APP_OP_CANCELLED && !t.valid &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_rp_count() == 1 && op_test_defer_end_count() == 1;
}

static bool t20(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    cleanup_arg_t c = {0};
    pthread_t th;
    app_operation_status_t st;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    c.op = A;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th, NULL, cleanup_thread, &c);
    if (!hook_wait_reached(2000)) {
        hook_release();
        pthread_join(th, NULL);
        return false;
    }
    app_operation_finish_result_t finish = app_operation_request_finish(
        A, a.gen, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    bool got = app_operation_get_status(A, &st);
    bool claim_was_running = claim_state(A, a.gen) == APP_OPERATION_CLEANUP_RUNNING;
    hook_release();
    pthread_join(th, NULL);

    return finish == APP_OPERATION_FINISH_DONE && got &&
           st.state == APP_OP_FAILED &&
           st.error == APP_OPERATION_ERR_CLEANUP_FAILED &&
           claim_was_running && c.result &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_rp_count() == 1 && op_test_defer_end_count() == 1;
}

static bool t21(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    finish_arg_t f = {0};
    app_operation_status_t st;
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    f.op = A;
    f.gen = a.gen;
    f.target_state = APP_OP_EXPIRED;
    f.target_error = APP_OPERATION_ERR_TIMEOUT;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th, NULL, finish_thread, &f);
    bool reached = hook_wait_reached(2000);
    app_operation_finish_result_t second = app_operation_request_finish(
        A, a.gen, APP_OP_CANCELLED, APP_OPERATION_ERR_CANCELLED,
        APP_OPERATION_CLEANUP_REQUIRED);
    hook_release();
    pthread_join(th, NULL);
    bool got = app_operation_get_status(A, &st);

    return reached && second == APP_OPERATION_FINISH_PENDING &&
           f.result == APP_OPERATION_FINISH_DONE && got &&
           st.state == APP_OP_EXPIRED &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_rp_count() == 1;
}

static bool t22(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    finish_arg_t f = {0};
    app_operation_status_t st;
    pthread_t th;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    f.op = A;
    f.gen = a.gen;
    f.target_state = APP_OP_CANCELLED;
    f.target_error = APP_OPERATION_ERR_CANCELLED;
    hook_wait_for("cleanup_after_claim");
    pthread_create(&th, NULL, finish_thread, &f);
    bool reached = hook_wait_reached(2000);
    app_operation_finish_result_t second = app_operation_request_finish(
        A, a.gen, APP_OP_EXPIRED, APP_OPERATION_ERR_TIMEOUT,
        APP_OPERATION_CLEANUP_REQUIRED);
    hook_release();
    pthread_join(th, NULL);
    bool got = app_operation_get_status(A, &st);

    return reached && second == APP_OPERATION_FINISH_PENDING &&
           f.result == APP_OPERATION_FINISH_DONE && got &&
           st.state == APP_OP_CANCELLED &&
           claim_state(A, a.gen) == APP_OPERATION_CLEANUP_OK &&
           job_test_rp_count() == 1;
}

static bool t23(void)
{
    run_ctx_t a;
    app_operation_t *A = NULL;
    char op_id[APP_OPERATION_ID_LEN];
    app_cmd_net_job_snap_t s;
    app_cmd_net_trial_snap_t t;

    init_all();
    if (!start_op(&a, &A, 30000, "A")) {
        return false;
    }
    snprintf(op_id, sizeof(op_id), "%s", app_operation_id(A));
    if (app_cmd_net_trial_store("A", "pass", APP_CMD_SOURCE_BLE,
                                op_id, NULL, A, NULL) != APP_CMD_NET_TRIAL_STORED) {
        return false;
    }
    app_operation_cleanup_claim_t claim = app_operation_cleanup_claim(A, a.gen);
    app_cmd_net_release_result_t rel = app_cmd_net_release_if_owned(A, a.gen);
    s = job_snapshot();
    t = trial_snapshot();
    bool running = claim_state(A, a.gen) == APP_OPERATION_CLEANUP_RUNNING;
    bool untouched = job_test_rp_count() == 0 && s.active && t.valid;
    app_operation_cleanup_complete(A, a.gen, true);

    return claim == APP_OPERATION_CLEANUP_CLAIMED &&
           rel == APP_CMD_NET_RELEASE_PENDING && running && untouched;
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */
typedef struct {
    const char *name;
    bool (*fn)(void);
} test_case_t;


#ifdef APP_C15_BUSY_DIAG
static int diag_format_tests(void)
{
    app_c15_busy_diag_t d;
    char buf[512];
    char job[160];
    uint32_t logged = 0;
    bool trunc = false;
    int failed = 0;

    memset(&d, 0, sizeof(d));
    d.ops.printed = 4;
    d.ops.active_ops = 4;
    for (int i = 0; i < 4; i++) {
        snprintf(d.ops.ops[i].id, sizeof(d.ops.ops[i].id),
                 "op-ABCDEFGHIJKLMNOP");
        d.ops.ops[i].state = APP_OP_CONNECTING;
        d.ops.ops[i].running = true;
        d.ops.ops[i].generation = (uint32_t)(100 + i);
        d.ops.ops[i].cleanup_state = APP_OPERATION_CLEANUP_RUNNING;
    }
    app_c15_busy_diag_format_ops(&d, buf, sizeof(buf), &logged, &trunc);
    if (logged != 4 || trunc || strstr(buf, "op-ABCDEFGHIJKLMNOP") == NULL) {
        printf("D01 full-list FAIL\n");
        failed++;
    } else {
        printf("D01 full-list PASS\n");
    }
    app_c15_busy_diag_format_ops(&d, buf, 40, &logged, &trunc);
    if (!trunc || logged == 4) {
        printf("D02 text-truncation FAIL\n");
        failed++;
    } else {
        printf("D02 text-truncation PASS\n");
    }
    d.ops.truncated = true;
    app_c15_busy_diag_format_ops(&d, buf, sizeof(buf), &logged, &trunc);
    if (!trunc) {
        printf("D03 count-truncation FAIL\n");
        failed++;
    } else {
        printf("D03 count-truncation PASS\n");
    }
    d.ops.truncated = false;
    d.job_op_valid = true;
    snprintf(d.ops.job_op.id, sizeof(d.ops.job_op.id), "op-TERMINAL0000000");
    d.ops.job_op.state = APP_OP_FAILED;
    d.ops.job_op.running = false;
    d.ops.job_op.generation = 7;
    d.ops.job_op.cleanup_state = APP_OPERATION_CLEANUP_OK;
    char expect_state[32];
    snprintf(expect_state, sizeof(expect_state), "job_op_state=%d",
             (int)APP_OP_FAILED);
    app_c15_busy_diag_format_job(&d, job, sizeof(job));
    if (strstr(job, "job_op_id=op-TERMINAL0000000") == NULL
        || strstr(job, expect_state) == NULL
        || strstr(job, "job_op_running=0") == NULL
        || strstr(job, "job_op_gen=7") == NULL
        || strstr(job, "job_op_cleanup=2") == NULL) {
        printf("D04 job-op-terminal FAIL\n");
        failed++;
    } else {
        printf("D04 job-op-terminal PASS\n");
    }
    d.job_op_valid = false;
    app_c15_busy_diag_format_job(&d, job, sizeof(job));
    const char *invalid_fields[] = {
        "job_op_id=-", "job_op_state=-", "job_op_running=-",
        "job_op_gen=-", "job_op_cleanup=-",
    };
    bool invalid_ok = true;
    for (size_t i = 0; i < sizeof(invalid_fields) / sizeof(invalid_fields[0]); i++) {
        if (strstr(job, invalid_fields[i]) == NULL) {
            invalid_ok = false;
        }
    }
    if (!invalid_ok) {
        printf("D05 job-op-invalid FAIL\n");
        failed++;
    } else {
        printf("D05 job-op-invalid PASS\n");
    }
    return failed;
}
#endif

int main(void)
{
    static const test_case_t cases[] = {
        {"T01", t01}, {"T02", t02}, {"T03", t03}, {"T04", t04},
        {"T05", t05}, {"T06", t06}, {"T07", t07}, {"T08", t08},
        {"T09a", t09a}, {"T09b", t09b}, {"T10", t10}, {"T11", t11},
        {"T12", t12}, {"T13", t13}, {"T14", t14}, {"T15", t15},
        {"T16", t16}, {"T17", t17}, {"T18", t18}, {"T19", t19},
        {"T20", t20}, {"T21", t21}, {"T22", t22}, {"T23", t23},
    };
    int failed = 0;

    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        pid_t pid = fork();
        if (pid == 0) {
            g_expected = false;
            bool ok = cases[i].fn();
            _exit(ok ? (g_expected ? 3 : 0) : 1);
        }
        if (pid < 0) {
            printf("%s FORK_FAIL\n", cases[i].name);
            failed++;
            continue;
        }
        int status = 0;
        if (waitpid(pid, &status, 0) < 0) {
            printf("%s WAIT_FAIL\n", cases[i].name);
            failed++;
            continue;
        }
        if (WIFEXITED(status) && WEXITSTATUS(status) == 0) {
            printf("%s PASS\n", cases[i].name);
        } else if (WIFEXITED(status) && WEXITSTATUS(status) == 3) {
            printf("%s EXPECTED\n", cases[i].name);
        } else {
            printf("%s FAIL\n", cases[i].name);
            failed++;
        }
    }
#ifdef APP_C15_BUSY_DIAG
    failed += diag_format_tests();
#endif
    printf("summary: %d failed\n", failed);
    return failed == 0 ? 0 : 1;
}
