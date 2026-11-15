#pragma once
#include <pthread.h>
#include <stddef.h>
#include <stdint.h>

typedef int BaseType_t;
typedef unsigned int TickType_t;
typedef void (*TaskFunction_t)(void *);
typedef void *TaskHandle_t;
typedef uint8_t StackType_t;
typedef struct { int unused; } StaticTask_t;

#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1

#define pdMS_TO_TICKS(ms) ((TickType_t)(ms))

#ifndef configSUPPORT_STATIC_ALLOCATION
#define configSUPPORT_STATIC_ALLOCATION 0
#endif

/* portMUX 桩：普通（非递归）mutex。临界区内重入会死锁——这本身是被测约束。 */
typedef struct { pthread_mutex_t m; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { .m = PTHREAD_MUTEX_INITIALIZER }

#ifdef __cplusplus
extern "C" {
#endif
void op_mux_enter(portMUX_TYPE *m);
void op_mux_exit(portMUX_TYPE *m);
#ifdef __cplusplus
}
#endif
#define portENTER_CRITICAL(m) op_mux_enter(m)
#define portEXIT_CRITICAL(m)  op_mux_exit(m)
