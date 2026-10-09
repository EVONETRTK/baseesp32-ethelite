// Stub dei message buffer FreeRTOS (host_rt.c): coda di messaggi in memoria.
#pragma once
#include "freertos/FreeRTOS.h"
typedef struct host_msgbuf *MessageBufferHandle_t;
typedef struct { void *p[4]; } StaticMessageBuffer_t;
MessageBufferHandle_t xMessageBufferCreate(size_t size);
MessageBufferHandle_t xMessageBufferCreateStatic(size_t size, uint8_t *storage, StaticMessageBuffer_t *st);
size_t xMessageBufferSend(MessageBufferHandle_t b, const void *data, size_t len, TickType_t t);
size_t xMessageBufferReceive(MessageBufferHandle_t b, void *out, size_t cap, TickType_t t);
