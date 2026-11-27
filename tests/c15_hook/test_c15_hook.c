/*
 * C15-2 test-only hook/defer Host matrix.
 *
 * 覆盖设计 rev3 的 H1–H27 中可在 host 上确定性构造的路径。不可构造的
 * 防御路径在输出中标记 NOT_COVERED，并由静态审查补充，不伪造 PASS。
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

static int g_fail;
static int g_not_covered;

static void report(const char *id, bool ok)
{
    printf("ASSERT %s=%s\n", id, ok ? "PASS" : "FAIL");
    if (!ok) {
        g_fail++;
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

static esp_err_t run_got_ip(app_operation_t *op, void *ctx)
{
    (void)ctx;
    return app_operation_set_state(op, APP_OP_GOT_IP, APP_OPERATION_ERR_NONE);
}

static esp_err_t run_connecting(app_operation_t *op, void *ctx)
{
    (void)op;
    (void)ctx;
    return ESP_OK;
}

static bool cleanup_ok(app_operation_t *op, void *ctx)
{
    (void)op;
    (void)ctx;
    return true;
}

static pthread_mutex_t g_finish_m = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t g_finish_c = PTHREAD_COND_INITIALIZER;
static int g_finish_entered;
static int g_finish_release;
static app_operation_t *g_finish_op;
static uint32_t g_finish_gen;
static app_operation_finish_result_t g_finish_result;

static app_operation_cleanup_outcome_t finish_block(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation)
{
    (void)op;
    (void)ctx;
    (void)policy;
    (void)expected_generation;
    pthread_mutex_lock(&g_finish_m);
    g_finish_entered = 1;
    pthread_cond_broadcast(&g_finish_c);
    while (!g_finish_release) {
        pthread_cond_wait(&g_finish_c, &g_finish_m);
    }
    pthread_mutex_unlock(&g_finish_m);
    return APP_OPERATION_CLEANUP_OUTCOME_DONE_OK;
}

static void *finish_thread(void *arg)
{
    (void)arg;
    g_finish_result = app_operation_request_finish(
        g_finish_op, g_finish_gen, APP_OP_CANCELLED,
        APP_OPERATION_ERR_CANCELLED, APP_OPERATION_CLEANUP_REQUIRED);
    return NULL;
}

static app_operation_cleanup_outcome_t finish_ok(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation)
{
    (void)op;
    (void)ctx;
    (void)policy;
    (void)expected_generation;
    return APP_OPERATION_CLEANUP_OUTCOME_DONE_OK;
}

static app_operation_cleanup_outcome_t finish_pending(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation)
{
    (void)op;
    (void)ctx;
    (void)policy;
    (void)expected_generation;
    return APP_OPERATION_CLEANUP_OUTCOME_PENDING;
}

static app_operation_cleanup_outcome_t finish_stale(
    app_operation_t *op, void *ctx, app_operation_cleanup_policy_t policy,
    uint32_t expected_generation)
{
    (void)op;
    (void)ctx;
    (void)policy;
    (void)expected_generation;
    return APP_OPERATION_CLEANUP_OUTCOME_STALE;
}

static app_operation_t *start_op(app_operation_owner_t owner,
                                 app_operation_finish_fn finish_fn,
                                 esp_err_t (*run_fn)(app_operation_t *, void *))
{
    app_operation_t *op = NULL;
    esp_err_t err;

    app_operation_init();
    err = app_operation_start_with_finish(owner, NULL, 20000, 300000,
                                          run_fn, cleanup_ok, finish_fn,
                                          NULL, &op);
    if (err != ESP_OK) {
        return NULL;
    }
    if (run_fn == run_got_ip && !wait_state(op, APP_OP_GOT_IP, 3000)) {
        return NULL;
    }
    if (run_fn == run_connecting && !wait_state(op, APP_OP_CONNECTING, 3000)) {
        return NULL;
    }
    return op;
}

static esp_err_t arm_target(app_operation_t *op, uint32_t hold_ms,
                            uint32_t *generation, int64_t *deadline_ms)
{
    return app_operation_test_hook_arm_target(app_operation_id(op), hold_ms, 0,
                                              generation, deadline_ms);
}

static bool hook_armed(void)
{
    app_operation_test_hook_status_t st;

    app_operation_test_hook_get_status(&st);
    return st.armed;
}

static bool event_contains(const char *needle)
{
    return strstr(op_test_hook_event_log(), needle) != NULL;
}

static void *expire_driver(void *arg)
{
    (void)arg;

    for (int i = 0; i < 8000 && !op_test_hook_entered(); i++) {
        usleep(1000);
    }
    if (!op_test_hook_entered()) {
        return NULL;
    }
    for (int i = 0; i < 8000 && !op_test_hook_exited(); i++) {
        op_test_advance_us(50 * 1000);
        usleep(1000);
    }
    return NULL;
}

static void h1_expire_hit(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;
    pthread_t drv;

    report("H1a", op != NULL);
    if (op == NULL) {
        return;
    }
    report("H1b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    pthread_create(&drv, NULL, expire_driver, NULL);
    op_test_advance_us(310000 * 1000);
    report("H1c", wait_state(op, APP_OP_EXPIRED, 5000));
    pthread_join(drv, NULL);
    report("H1d", op_test_hook_entered());
    report("H1e", !hook_armed());
}

static void h2_wrong_id(void)
{
    uint32_t gen = 0;
    int64_t deadline = 0;

    app_operation_init();
    report("H2", app_operation_test_hook_arm_target("op-deadbeef", 360000, 0,
                                                    &gen, &deadline) ==
                    ESP_ERR_NOT_FOUND);
}

static void h3_not_got_ip(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_connecting);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H3a", op != NULL);
    if (op == NULL) {
        return;
    }
    report("H3b", arm_target(op, 360000, &gen, &deadline) ==
                    ESP_ERR_INVALID_STATE);
}

static void h4_finish_registered(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_block, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;
    pthread_t th;

    report("H4a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    pthread_mutex_lock(&g_finish_m);
    g_finish_entered = 0;
    g_finish_release = 0;
    pthread_mutex_unlock(&g_finish_m);
    g_finish_op = op;
    g_finish_gen = st.generation;
    pthread_create(&th, NULL, finish_thread, NULL);
    pthread_mutex_lock(&g_finish_m);
    while (!g_finish_entered) {
        pthread_cond_wait(&g_finish_c, &g_finish_m);
    }
    pthread_mutex_unlock(&g_finish_m);

    /* finish 已登记、callback 尚在飞；目标必须拒绝 arm。 */
    report("H4b", arm_target(op, 360000, &gen, &deadline) ==
                    ESP_ERR_INVALID_STATE);

    pthread_mutex_lock(&g_finish_m);
    g_finish_release = 1;
    pthread_cond_broadcast(&g_finish_c);
    pthread_mutex_unlock(&g_finish_m);
    pthread_join(th, NULL);
    report("H4c", g_finish_result == APP_OPERATION_FINISH_DONE &&
                    wait_state(op, APP_OP_CANCELLED, 3000));
}

static void h5_global_hook_blocks_target(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H5a", op != NULL);
    if (op == NULL) {
        return;
    }
    app_operation_test_hook_arm(2000, 0);
    report("H5b", arm_target(op, 360000, &gen, &deadline) ==
                    ESP_ERR_INVALID_STATE);
    app_operation_test_hook_clear();
}

static void h6_request_finish_cancel_clears(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_ok, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H6a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    report("H6b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H6c", app_operation_request_finish(
                      op, st.generation, APP_OP_CANCELLED,
                      APP_OPERATION_ERR_CANCELLED,
                      APP_OPERATION_CLEANUP_REQUIRED) ==
                      APP_OPERATION_FINISH_DONE);
    report("H6d", !hook_armed());
    report("H6e", event_contains("target_cancelled"));
}

static void h7_cancel_pause_clears(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H7a", op != NULL);
    if (op == NULL) {
        return;
    }
    report("H7b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H7c", app_operation_cancel(op) == ESP_OK);
    report("H7d", !hook_armed());
    report("H7e", event_contains("target_cancelled"));
}

static void h8_rearm_new_target(void)
{
    app_operation_t *first = start_op(APP_OPERATION_OWNER_BLE, finish_ok, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;
    app_operation_t *second;

    report("H8a", first != NULL && app_operation_get_status(first, &st));
    if (first == NULL) {
        return;
    }
    report("H8b", arm_target(first, 360000, &gen, &deadline) == ESP_OK);
    (void)app_operation_request_finish(first, st.generation, APP_OP_CANCELLED,
                                       APP_OPERATION_ERR_CANCELLED,
                                       APP_OPERATION_CLEANUP_REQUIRED);
    second = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    report("H8c", second != NULL);
    if (second != NULL) {
        report("H8d", arm_target(second, 360000, &gen, &deadline) == ESP_OK);
    }
}

static void h9_non_target_notify_skip(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H9a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    report("H9b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    app_operation_test_hook_notify_target_inactive_for_test("op-other", st.generation);
    report("H9c", hook_armed());
    app_operation_test_hook_clear();
}

static void h11_set_state_terminal(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H11a", op != NULL);
    if (op == NULL) {
        return;
    }
    report("H11b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H11c", app_operation_set_state(op, APP_OP_CANCELLED,
                                           APP_OPERATION_ERR_CANCELLED) == ESP_OK);
    report("H11d", !hook_armed());
    report("H11e", event_contains("target_cancelled"));
}

static void h12_clear(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H12a", op != NULL);
    if (op == NULL) {
        return;
    }
    report("H12b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    app_operation_test_hook_clear();
    report("H12c", !hook_armed());
}

static void h14_timeout_then_arm(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H14a", op != NULL);
    if (op == NULL) {
        return;
    }
    app_operation_test_hook_arm(100, 0);
    op_test_advance_us(200 * 1000);
    report("H14b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H14c", hook_armed());
    app_operation_test_hook_clear();
}

typedef struct {
    app_operation_t *op;
    uint32_t generation;
    app_operation_finish_result_t result;
} h15_arg_t;

static void *h15_finish_thread(void *v)
{
    h15_arg_t *a = (h15_arg_t *)v;

    a->result = app_operation_request_finish(
        a->op, a->generation, APP_OP_EXPIRED, APP_OPERATION_ERR_TIMEOUT,
        APP_OPERATION_CLEANUP_REQUIRED);
    return NULL;
}

typedef struct {
    pthread_barrier_t *barrier;
    const char *op_id;
    uint32_t generation;
    int role;
    int loops;
} h15_race_arg_t;

static void *h15_race_worker(void *v)
{
    h15_race_arg_t *a = (h15_race_arg_t *)v;

    for (int i = 0; i < a->loops; i++) {
        pthread_barrier_wait(a->barrier);
        if (a->role == 0) {
            uint32_t g = 0;
            int64_t d = 0;
            (void)app_operation_test_hook_arm_target(a->op_id, 360000, 0,
                                                     &g, &d);
        } else if (a->role == 1) {
            app_operation_test_hook_clear();
        } else {
            app_operation_test_hook_notify_target_inactive_for_test("op-other",
                                                                    a->generation);
        }
        pthread_barrier_wait(a->barrier);
    }
    return NULL;
}

static bool h15_hook_state_valid(const char *op_id, uint32_t generation)
{
    app_operation_test_hook_status_t hs;

    app_operation_test_hook_get_status(&hs);
    if (!hs.armed) {
        return true;
    }
    return hs.targeted && hs.generation == generation &&
           strcmp(hs.op_id, op_id) == 0;
}

static void h15_concurrent(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_ok, run_got_ip);
    app_operation_status_t st;
    app_operation_test_hook_status_t hs;
    uint32_t gen = 0;
    uint32_t rearm_gen = 0;
    int64_t deadline = 0;
    int64_t rearm_deadline = 0;
    char op_id[APP_OPERATION_ID_LEN];
    pthread_t finisher;
    pthread_t drv;
    h15_arg_t arg;
    app_operation_t *op2 = NULL;
    app_operation_status_t st2;
    char op2_id[APP_OPERATION_ID_LEN] = "";
    pthread_barrier_t race_bar;
    pthread_t race_th[3];
    h15_race_arg_t race_args[3];
    bool race_ok = true;
    uint32_t race_gen = 0;

    report("H15a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    memcpy(op_id, app_operation_id(op), sizeof(op_id));
    report("H15b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    arg.op = op;
    arg.generation = st.generation;
    arg.result = APP_OPERATION_FINISH_REJECTED;
    pthread_create(&finisher, NULL, h15_finish_thread, &arg);

    /* 等待真实 expire pause 进入；此时 hook 已被 pause 消费。 */
    for (int i = 0; i < 2000 && !op_test_hook_entered(); i++) {
        usleep(1000);
    }
    report("H15c", op_test_hook_entered());
    app_operation_test_hook_get_status(&hs);
    report("H15d", !hs.armed);

    /* pause 窗口内交错 arm / clear / notify；三者都不得复活 hook。 */
    app_operation_test_hook_clear();
    app_operation_test_hook_notify_target_inactive_for_test(op_id, st.generation);
    report("H15e", app_operation_test_hook_arm_target(op_id, 360000, 0,
                                                      &rearm_gen,
                                                      &rearm_deadline) ==
                    ESP_ERR_INVALID_STATE);
    app_operation_test_hook_get_status(&hs);
    report("H15f", !hs.armed);

    pthread_create(&drv, NULL, expire_driver, NULL);
    pthread_join(finisher, NULL);
    pthread_join(drv, NULL);

    report("H15g", arg.result == APP_OPERATION_FINISH_DONE);
    report("H15h", wait_state(op, APP_OP_EXPIRED, 3000));
    report("H15i", op_test_hook_pause_count() == 1);
    /* 关键：在任何 teardown/clear 之前检查最终 hook 状态。 */
    app_operation_test_hook_get_status(&hs);
    report("H15j", !hs.armed && !hs.targeted);

    /* 并发阶段：新目标保持 GOT_IP，arm/clear/notify 三线程同时竞争 O。
     * 不变量：每轮结束后要么 disarmed，要么 armed 且指向本轮有效目标。 */
    op2 = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    report("H15k", op2 != NULL && app_operation_get_status(op2, &st2));
    if (op2 == NULL) {
        return;
    }
    memcpy(op2_id, app_operation_id(op2), sizeof(op2_id));
    race_gen = st2.generation;
    pthread_barrier_init(&race_bar, NULL, 4);
    for (int i = 0; i < 3; i++) {
        race_args[i].barrier = &race_bar;
        race_args[i].op_id = op2_id;
        race_args[i].generation = race_gen;
        race_args[i].role = i;
        race_args[i].loops = 200;
        pthread_create(&race_th[i], NULL, h15_race_worker, &race_args[i]);
    }
    for (int i = 0; i < 200; i++) {
        pthread_barrier_wait(&race_bar);
        pthread_barrier_wait(&race_bar);
        if (!h15_hook_state_valid(op2_id, race_gen)) {
            race_ok = false;
        }
    }
    for (int i = 0; i < 3; i++) {
        pthread_join(race_th[i], NULL);
    }
    pthread_barrier_destroy(&race_bar);
    report("H15l", race_ok);
    report("H15m", h15_hook_state_valid(op2_id, race_gen));
    report("H15n", op_test_hook_pause_count() == 1);

    /* 记录完成后的显式 teardown；断言已在 clear 之前完成。 */
    app_operation_test_hook_clear();
    report("H15o", !hook_armed());
}

static void h10_slot_reuse(void)
{
    app_operation_t *first = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    app_operation_status_t st;
    app_operation_t *second;
    app_operation_test_hook_status_t hs;
    uint32_t gen = 0;
    int64_t deadline = 0;
    char first_id[APP_OPERATION_ID_LEN];

    report("H10a", first != NULL && app_operation_get_status(first, &st));
    if (first == NULL) {
        return;
    }
    memcpy(first_id, app_operation_id(first), sizeof(first_id));
    report("H10b", app_operation_set_state(first, APP_OP_CANCELLED,
                                           APP_OPERATION_ERR_CANCELLED) == ESP_OK);
    app_operation_test_hook_force_target_for_test(first_id, st.generation,
                                                  op_test_now_us() / 1000 + 360000);
    second = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    report("H10c", second != NULL);
    if (second == NULL) {
        return;
    }
    report("H10d", arm_target(second, 360000, &gen, &deadline) == ESP_OK);
    app_operation_test_hook_get_status(&hs);
    report("H10e", hs.armed && hs.targeted &&
                    strcmp(hs.op_id, app_operation_id(second)) == 0);
    app_operation_test_hook_clear();
}

static void h13_pause_timeout(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_ok, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;
    int64_t now_ms;
    int64_t remaining_ms;
    uint32_t hold_ms;

    report("H13a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    now_ms = op_test_now_us() / 1000;
    remaining_ms = st.deadline_ms - now_ms;
    hold_ms = (uint32_t)(remaining_ms + 11000);
    report("H13b", arm_target(op, hold_ms, &gen, &deadline) == ESP_OK);
    op_test_advance_us((remaining_ms + 15000) * 1000);
    report("H13c", wait_state(op, APP_OP_EXPIRED, 5000));
    report("H13d", !op_test_hook_entered());
    report("H13e", !hook_armed());
    report("H13f", event_contains("timeout"));
}

typedef struct {
    app_operation_t *op;
    uint32_t generation;
    app_operation_finish_result_t result;
    pthread_barrier_t *barrier;
} h16_arg_t;

static void *h16_finish_thread(void *v)
{
    h16_arg_t *a = (h16_arg_t *)v;

    pthread_barrier_wait(a->barrier);
    a->result = app_operation_request_finish(
        a->op, a->generation, APP_OP_EXPIRED, APP_OPERATION_ERR_TIMEOUT,
        APP_OPERATION_CLEANUP_REQUIRED);
    return NULL;
}

static void h16_worker_expire_race(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_ok, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;
    pthread_t th[2];
    pthread_t drv;
    pthread_barrier_t barrier;
    h16_arg_t args[2];

    report("H16a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    report("H16b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    pthread_barrier_init(&barrier, NULL, 2);
    for (int i = 0; i < 2; i++) {
        args[i].op = op;
        args[i].generation = st.generation;
        args[i].result = APP_OPERATION_FINISH_REJECTED;
        args[i].barrier = &barrier;
        pthread_create(&th[i], NULL, h16_finish_thread, &args[i]);
    }
    pthread_create(&drv, NULL, expire_driver, NULL);
    for (int i = 0; i < 2; i++) {
        pthread_join(th[i], NULL);
    }
    pthread_join(drv, NULL);
    pthread_barrier_destroy(&barrier);
    report("H16c", (args[0].result == APP_OPERATION_FINISH_DONE &&
                    args[1].result == APP_OPERATION_FINISH_PENDING) ||
                    (args[1].result == APP_OPERATION_FINISH_DONE &&
                     args[0].result == APP_OPERATION_FINISH_PENDING));
    report("H16d", wait_state(op, APP_OP_EXPIRED, 3000));
    report("H16e", op_test_hook_entered());
    report("H16f", !hook_armed());
}

static void h17_defer_invalid_state(void)
{
    app_operation_start_diag_t diag;
    app_operation_t *op = NULL;
    esp_err_t err;

    app_operation_init();
    op_test_reset_defer_counts();
    op_test_set_defer_begin_result(ESP_ERR_INVALID_STATE);
    err = app_operation_start_with_finish_diag(
        APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
        run_got_ip, cleanup_ok, NULL, NULL, &op, &diag);
    report("H17a", err == ESP_ERR_INVALID_STATE);
    report("H17b", diag.defer_attempted && !diag.defer_started);
    report("H17c", diag.defer_result == ESP_ERR_INVALID_STATE);
    report("H17d", !diag.active_check_done);
    report("H17e", diag.start_result == ESP_ERR_INVALID_STATE);
}

static void h18_defer_active_operation(void)
{
    app_operation_start_diag_t diag;
    app_operation_t *first = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    app_operation_t *second = NULL;
    esp_err_t err;

    report("H18a", first != NULL);
    if (first == NULL) {
        return;
    }
    op_test_set_defer_begin_result(ESP_OK);
    err = app_operation_start_with_finish_diag(
        APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
        run_got_ip, cleanup_ok, NULL, NULL, &second, &diag);
    report("H18b", err == ESP_ERR_INVALID_STATE);
    report("H18c", diag.defer_attempted && diag.defer_started);
    report("H18d", diag.active_check_done && diag.active_check_result);
    report("H18e", diag.start_result == ESP_ERR_INVALID_STATE);
}

static void h19_defer_no_mem(void)
{
    app_operation_start_diag_t diag;
    app_operation_t *op = NULL;
    esp_err_t err;

    app_operation_init();
    op_test_reset_defer_counts();
    op_test_set_defer_begin_result(ESP_ERR_NO_MEM);
    err = app_operation_start_with_finish_diag(
        APP_OPERATION_OWNER_BLE, NULL, 20000, 300000,
        run_got_ip, cleanup_ok, NULL, NULL, &op, &diag);
    report("H19a", err == ESP_ERR_NO_MEM);
    report("H19b", diag.defer_result == ESP_ERR_NO_MEM);
    report("H19c", !diag.defer_started);
    report("H19d", diag.start_result == ESP_ERR_NO_MEM);
}

static void h20_internal_owner(void)
{
    app_operation_start_diag_t diag;
    app_operation_t *op = NULL;
    esp_err_t err;

    app_operation_init();
    op_test_reset_defer_counts();
    err = app_operation_start_with_finish_diag(
        APP_OPERATION_OWNER_INTERNAL, NULL, 20000, 300000,
        run_got_ip, cleanup_ok, NULL, NULL, &op, &diag);
    report("H20a", err == ESP_OK);
    report("H20b", !diag.defer_attempted);
    report("H20c", diag.active_check_done && !diag.active_check_result);
    report("H20d", diag.start_result == ESP_OK);
}

static void h21_static_snapshot(void)
{
    printf("ASSERT H21=NOT_COVERED (static review: no op-> read after O unlock)\n");
    g_not_covered = 1;
}

static void h22_hold_too_short(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H22a", op != NULL);
    if (op == NULL) {
        return;
    }
    report("H22b", arm_target(op, 100, &gen, &deadline) == ESP_ERR_INVALID_ARG);
    report("H22c", !hook_armed());
}

static void h23_pending(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_pending, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H23a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    report("H23b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H23c", app_operation_request_finish(
                      op, st.generation, APP_OP_CANCELLED,
                      APP_OPERATION_ERR_CANCELLED,
                      APP_OPERATION_CLEANUP_REQUIRED) ==
                      APP_OPERATION_FINISH_PENDING);
    report("H23d", hook_armed());
    app_operation_test_hook_clear();
}

static void h24_stale(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, finish_stale, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H24a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    report("H24b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H24c", app_operation_request_finish(
                      op, st.generation, APP_OP_CANCELLED,
                      APP_OPERATION_ERR_CANCELLED,
                      APP_OPERATION_CLEANUP_REQUIRED) ==
                      APP_OPERATION_FINISH_STALE);
    report("H24d", hook_armed());
    app_operation_test_hook_clear();
}

static void h25_by_value_notify(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;
    char op_id[APP_OPERATION_ID_LEN];

    report("H25a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    memcpy(op_id, app_operation_id(op), sizeof(op_id));
    report("H25b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    app_operation_test_hook_notify_target_inactive_for_test(op_id, gen + 1);
    report("H25c", hook_armed());
    app_operation_test_hook_notify_target_inactive_for_test(op_id, gen);
    report("H25d", !hook_armed());
    report("H25e", event_contains("target_terminal"));
}

static void h26_cleanup_complete(void)
{
    app_operation_t *op = start_op(APP_OPERATION_OWNER_BLE, NULL, run_got_ip);
    app_operation_status_t st;
    uint32_t gen = 0;
    int64_t deadline = 0;

    report("H26a", op != NULL && app_operation_get_status(op, &st));
    if (op == NULL) {
        return;
    }
    report("H26b", arm_target(op, 360000, &gen, &deadline) == ESP_OK);
    report("H26c", app_operation_cleanup_claim(op, st.generation) ==
                    APP_OPERATION_CLEANUP_CLAIMED);
    app_operation_cleanup_complete(op, st.generation, true);
    report("H26d", hook_armed());
    app_operation_test_hook_clear();
}

static void h27_apply_failure(void)
{
    printf("ASSERT H27=NOT_COVERED (apply failure is not constructible via public API)\n");
    g_not_covered = 1;
}

typedef void (*case_fn)(void);

static int run_case(const char *name, case_fn fn)
{
    pid_t pid;
    int status;

    fflush(stdout);
    pid = fork();
    if (pid == 0) {
        g_fail = 0;
        g_not_covered = 0;
        fn();
        printf("CASE %s %s\n", name,
               g_fail != 0 ? "FAIL" : (g_not_covered ? "NOT_COVERED" : "PASS"));
        fflush(stdout);
        _exit(g_fail != 0 ? 1 : (g_not_covered ? 2 : 0));
    }
    if (pid < 0) {
        printf("CASE %s FORK_FAIL\n", name);
        return 0;
    }
    (void)waitpid(pid, &status, 0);
    if (!WIFEXITED(status)) {
        return 0;
    }
    if (WEXITSTATUS(status) == 0) {
        return 1;
    }
    return WEXITSTATUS(status) == 2 ? 2 : 0;
}

int main(void)
{
    int all = 1;
    int gaps = 0;
#define RUN_CASE(label, fn) do { \
    int rc_ = run_case(label, fn); \
    if (rc_ == 0) { all = 0; } \
    if (rc_ == 2) { gaps = 1; } \
} while (0)

    setvbuf(stdout, NULL, _IONBF, 0);
    RUN_CASE("H1_expire_hit", h1_expire_hit);
    RUN_CASE("H2_wrong_id", h2_wrong_id);
    RUN_CASE("H3_not_got_ip", h3_not_got_ip);
    RUN_CASE("H4_finish_registered", h4_finish_registered);
    RUN_CASE("H5_global_blocks", h5_global_hook_blocks_target);
    RUN_CASE("H6_request_finish_cancel", h6_request_finish_cancel_clears);
    RUN_CASE("H7_cancel_pause", h7_cancel_pause_clears);
    RUN_CASE("H8_rearm", h8_rearm_new_target);
    RUN_CASE("H9_non_target", h9_non_target_notify_skip);
    RUN_CASE("H10_slot_reuse", h10_slot_reuse);
    RUN_CASE("H11_set_state_terminal", h11_set_state_terminal);
    RUN_CASE("H12_clear", h12_clear);
    RUN_CASE("H13_pause_timeout", h13_pause_timeout);
    RUN_CASE("H14_timeout_then_arm", h14_timeout_then_arm);
    RUN_CASE("H15_concurrent", h15_concurrent);
    RUN_CASE("H16_worker_expire_race", h16_worker_expire_race);
    RUN_CASE("H17_defer_invalid", h17_defer_invalid_state);
    RUN_CASE("H18_defer_active", h18_defer_active_operation);
    RUN_CASE("H19_defer_no_mem", h19_defer_no_mem);
    RUN_CASE("H20_internal", h20_internal_owner);
    RUN_CASE("H21_static", h21_static_snapshot);
    RUN_CASE("H22_hold_short", h22_hold_too_short);
    RUN_CASE("H23_pending", h23_pending);
    RUN_CASE("H24_stale", h24_stale);
    RUN_CASE("H25_by_value", h25_by_value_notify);
    RUN_CASE("H26_cleanup_complete", h26_cleanup_complete);
    RUN_CASE("H27_apply_failure", h27_apply_failure);
#undef RUN_CASE

    if (!all) {
        printf("RESULT: FAIL\n");
        return 1;
    }
    printf("RESULT: %s\n", gaps ? "PASS_WITH_NOT_COVERED" : "PASS");
    return 0;
}
