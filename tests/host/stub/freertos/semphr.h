// Stub dei semafori FreeRTOS: un solo thread, prendere/dare riesce sempre.
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct host_sem *SemaphoreHandle_t;
SemaphoreHandle_t xSemaphoreCreateMutex(void);
BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t);
BaseType_t xSemaphoreGive(SemaphoreHandle_t s);
