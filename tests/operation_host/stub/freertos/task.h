#pragma once
#include "FreeRTOS.h"

#ifdef __cplusplus
extern "C" {
#endif
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack,
                       void *arg, unsigned prio, TaskHandle_t *out);
void vTaskDelay(TickType_t ticks);
void xTaskNotifyGive(TaskHandle_t t);
uint32_t ulTaskNotifyTake(BaseType_t clear, TickType_t ticks);
#ifdef __cplusplus
}
#endif
