// Stub delle code FreeRTOS (host_rt.c): coda in memoria, senza attese vere.
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct host_queue *QueueHandle_t;
QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size);
BaseType_t xQueueReset(QueueHandle_t q);
BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item);
BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t);
BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t t);
