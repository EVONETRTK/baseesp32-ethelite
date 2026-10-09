// Stub di freertos/stream_buffer.h per il fuzzing sul PC (ntrip_client.c).
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct host_stream *StreamBufferHandle_t;
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *buf, size_t len, TickType_t wait);
TickType_t xTaskGetTickCount(void); // in FreeRTOS sta in task.h
