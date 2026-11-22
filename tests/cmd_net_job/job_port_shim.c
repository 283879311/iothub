#define _GNU_SOURCE
#include "job_port_shim.h"

#include <pthread.h>
#include <stdatomic.h>
#include <string.h>

#include "cmd_net_job.h"
#include "rtos_shim.h"

static pthread_mutex_t s_job_lock;
static pthread_mutex_t s_cfg_lock;
static bool s_ble_connected = true;
static bool s_cleanup_result = true;
/* 并发 cleanup/断连路径会从多个线程调用 port；计数器用原子避免数据竞争。 */
static _Atomic int s_rp;
static _Atomic int s_tc;
static _Atomic int s_nc;
static _Atomic int s_clear;

static void ports_init(void)
{
    pthread_mutexattr_t attr;

    pthread_mutexattr_init(&attr);
    pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
    pthread_mutex_init(&s_job_lock, &attr);
    pthread_mutex_init(&s_cfg_lock, &attr);
    pthread_mutexattr_destroy(&attr);
}

static void lock_init_once(void)
{
    static pthread_once_t once = PTHREAD_ONCE_INIT;

    pthread_once(&once, ports_init);
}

static void job_lock(void) { pthread_mutex_lock(&s_job_lock); }
static void job_unlock(void) { pthread_mutex_unlock(&s_job_lock); }
static bool ble_connected(void) { return s_ble_connected; }

static bool cleanup_request(const char *op_id)
{
    (void)op_id;
    atomic_fetch_add(&s_rp, 1);
    return s_cleanup_result;
}

static bool try_cancel(void)
{
    atomic_fetch_add(&s_tc, 1);
    return true;
}

static void clear_active_operation(const char *op_id)
{
    (void)op_id;
    atomic_fetch_add(&s_clear, 1);
}

static void netcheck_cancel(uint32_t link_gen)
{
    (void)link_gen;
    atomic_fetch_add(&s_nc, 1);
}

static int64_t now_ms(void)
{
    return op_test_now_us() / 1000;
}

void job_test_port_reset(void)
{
    lock_init_once();
    s_ble_connected = true;
    s_cleanup_result = true;
    atomic_store(&s_rp, 0);
    atomic_store(&s_tc, 0);
    atomic_store(&s_nc, 0);
    atomic_store(&s_clear, 0);
}

void job_test_port_install(void)
{
    const app_cmd_net_job_port_t port = {
        .lock = job_lock,
        .unlock = job_unlock,
        .ble_connected = ble_connected,
        .cleanup_request = cleanup_request,
        .try_cancel = try_cancel,
        .clear_active_operation = clear_active_operation,
        .netcheck_cancel = netcheck_cancel,
        .now_ms = now_ms,
    };

    (void)app_cmd_net_job_init_with_port(&port);
}

void job_test_set_ble_connected(bool on) { s_ble_connected = on; }
void job_test_set_cleanup_result(bool ok) { s_cleanup_result = ok; }
int job_test_rp_count(void) { return atomic_load(&s_rp); }
int job_test_tc_count(void) { return atomic_load(&s_tc); }
int job_test_nc_count(void) { return atomic_load(&s_nc); }
int job_test_clear_count(void) { return atomic_load(&s_clear); }

/* cmd_cfg_txn.c 的配置锁依赖：host 使用独立递归互斥。 */
void app_config_lock(void) { pthread_mutex_lock(&s_cfg_lock); }
void app_config_unlock(void) { pthread_mutex_unlock(&s_cfg_lock); }
