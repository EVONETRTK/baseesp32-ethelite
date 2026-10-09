// Stub dei task FreeRTOS: sul PC nessun task parte davvero.
#pragma once
#include "freertos/FreeRTOS.h"
typedef void (*TaskFunction_t)(void *);
typedef struct host_task *TaskHandle_t;
void vTaskDelay(TickType_t t);
BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h);
BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h, BaseType_t core);
UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h);
