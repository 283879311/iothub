#define _GNU_SOURCE
#include <pthread.h>
#include <stdarg.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "rtos_shim.h"

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

/* ------------------------------------------------------------------ */
/* 虚拟时钟（微秒）                                                    */
/* ------------------------------------------------------------------ */
static pthread_mutex_t s_clock_m = PTHREAD_MUTEX_INITIALIZER;
static int64_t s_now_us = 0;

int64_t op_test_now_us(void)
{
    int64_t v;
    pthread_mutex_lock(&s_clock_m);
    v = s_now_us;
    pthread_mutex_unlock(&s_clock_m);
    return v;
}

void op_test_advance_us(int64_t us)
{
    pthread_mutex_lock(&s_clock_m);
    s_now_us += us;
    pthread_mutex_unlock(&s_clock_m);
}

/* ------------------------------------------------------------------ */
/* hook 事件与日志识别                                                 */
/* ------------------------------------------------------------------ */
static pthread_mutex_t s_ev_m = PTHREAD_MUTEX_INITIALIZER;
static int s_entered = 0;
static int s_exited = 0;
static char s_probe[1024] = "";
static int s_verbose = 0;

static void ev_lock(void) { pthread_mutex_lock(&s_ev_m); }
static void ev_unlock(void) { pthread_mutex_unlock(&s_ev_m); }

bool op_test_hook_entered(void)
{
    int v;
    ev_lock(); v = s_entered; ev_unlock();
    return v != 0;
}

bool op_test_hook_exited(void)
{
    int v;
    ev_lock(); v = s_exited; ev_unlock();
    return v != 0;
}

const char *op_test_hook_probe_log(void)
{
    return s_probe;
}

void op_test_set_verbose(int on)
{
    s_verbose = on;
}

/* 识别产品代码既有打印，不新增产品接口 */
static void note_log(const char *msg)
{
    if (strstr(msg, "C1 hook: pause") != NULL) {
        if (strstr(msg, " enter") != NULL) {
            ev_lock(); s_entered = 1; ev_unlock();
        }
        if (strstr(msg, " exit") != NULL) {
            ev_lock(); s_exited = 1; ev_unlock();
        }
    }
    if (strstr(msg, "probe set_state(") != NULL) {
        ev_lock();
        snprintf(s_probe, sizeof(s_probe), "%s", msg);
        ev_unlock();
    }
}

void op_test_log(char level, const char *tag, const char *fmt, ...)
{
    char buf[1024];
    va_list ap;

    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);

    note_log(buf);
    if (s_verbose) {
        fprintf(stderr, "[%c %s] %s\n", level, tag ? tag : "-", buf);
    }
}

/* ------------------------------------------------------------------ */
/* portMUX 桩：普通 mutex（重入即死锁，属被测约束）                     */
/* ------------------------------------------------------------------ */
void op_mux_enter(portMUX_TYPE *m) { pthread_mutex_lock(&m->m); }
void op_mux_exit(portMUX_TYPE *m) { pthread_mutex_unlock(&m->m); }

/* ------------------------------------------------------------------ */
/* FreeRTOS 任务桩                                                     */
/* ------------------------------------------------------------------ */
struct op_task {
    pthread_mutex_t m;
    pthread_cond_t c;
    int count;
};

static __thread struct op_task *t_self;

static void *op_task_trampoline(void *p)
{
    struct {
        TaskFunction_t fn;
        void *arg;
        struct op_task *t;
    } *start = p;
    TaskFunction_t fn = start->fn;
    void *arg = start->arg;

    t_self = start->t;
    free(start);
    fn(arg);
    return NULL;
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, unsigned prio, TaskHandle_t *out)
{
    struct op_task *t;
    pthread_t th;
    struct {
        TaskFunction_t fn;
        void *arg;
        struct op_task *t;
    } *start;

    (void)name; (void)stack; (void)prio;

    t = calloc(1, sizeof(*t));
    if (t == NULL) {
        return pdFALSE;
    }
    pthread_mutex_init(&t->m, NULL);
    pthread_cond_init(&t->c, NULL);

    start = calloc(1, sizeof(*start));
    if (start == NULL) {
        free(t);
        return pdFALSE;
    }
    start->fn = fn;
    start->arg = arg;
    start->t = t;

    if (pthread_create(&th, NULL, op_task_trampoline, start) != 0) {
        free(start);
        free(t);
        return pdFALSE;
    }
    pthread_detach(th);
    if (out != NULL) {
        *out = t;
    }
    return pdPASS;
}

void vTaskDelay(TickType_t ticks)
{
    int64_t until = op_test_now_us() + (int64_t)ticks * 1000;

    while (op_test_now_us() < until) {
        usleep(1000);
    }
}

void xTaskNotifyGive(TaskHandle_t h)
{
    struct op_task *t = (struct op_task *)h;

    if (t == NULL) {
        return;
    }
    pthread_mutex_lock(&t->m);
    t->count++;
    pthread_cond_signal(&t->c);
    pthread_mutex_unlock(&t->m);
}

uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks)
{
    struct op_task *t = t_self;
    int64_t deadline;
    uint32_t taken = 0;

    if (t == NULL) {
        return 0;
    }
    deadline = op_test_now_us() + (int64_t)ticks * 1000;

    pthread_mutex_lock(&t->m);
    for (;;) {
        if (t->count > 0) {
            taken = (uint32_t)t->count;
            if (clear) {
                t->count = 0;
            }
            break;
        }
        if (op_test_now_us() >= deadline) {
            break;
        }
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        ts.tv_nsec += 5 * 1000 * 1000;
        if (ts.tv_nsec >= 1000000000L) { ts.tv_sec++; ts.tv_nsec -= 1000000000L; }
        pthread_cond_timedwait(&t->c, &t->m, &ts);
    }
    pthread_mutex_unlock(&t->m);
    return taken;
}

/* ------------------------------------------------------------------ */
/* ESP-IDF 符号桩                                                      */
/* ------------------------------------------------------------------ */
static _Atomic uint32_t s_rand = 0x12345678u;

uint32_t esp_random(void)
{
    return atomic_fetch_add(&s_rand, 0x9E3779B9u) ^ 0x5A5A5A5Au;
}

int64_t esp_timer_get_time(void)
{
    return op_test_now_us();
}

const char *esp_err_to_name(esp_err_t err)
{
    switch (err) {
    case ESP_OK: return "ESP_OK";
    case ESP_FAIL: return "ESP_FAIL";
    case ESP_ERR_NO_MEM: return "ESP_ERR_NO_MEM";
    case ESP_ERR_INVALID_ARG: return "ESP_ERR_INVALID_ARG";
    case ESP_ERR_INVALID_STATE: return "ESP_ERR_INVALID_STATE";
    case ESP_ERR_NOT_FOUND: return "ESP_ERR_NOT_FOUND";
    case ESP_ERR_NOT_SUPPORTED: return "ESP_ERR_NOT_SUPPORTED";
    case ESP_ERR_TIMEOUT: return "ESP_ERR_TIMEOUT";
    default: return "ESP_ERR_UNKNOWN";
    }
}

size_t heap_caps_get_largest_free_block(uint32_t caps)
{
    (void)caps;
    return 0;
}

static _Atomic int s_defer_begin_count;
static _Atomic int s_defer_end_count;

void op_test_reset_defer_counts(void)
{
    atomic_store(&s_defer_begin_count, 0);
    atomic_store(&s_defer_end_count, 0);
}

int op_test_defer_begin_count(void)
{
    return atomic_load(&s_defer_begin_count);
}

int op_test_defer_end_count(void)
{
    return atomic_load(&s_defer_end_count);
}

esp_err_t app_mqtt_defer_restart_begin(void)
{
    atomic_fetch_add(&s_defer_begin_count, 1);
    return ESP_OK;
}

void app_mqtt_defer_restart_end_at(const char *tag)
{
    (void)tag;
    atomic_fetch_add(&s_defer_end_count, 1);
}
