// Stub minimo di FreeRTOS per le prove sul PC (un solo thread: mutex e
// sezioni critiche non fanno nulla). Non e' l'header di ESP-IDF.
#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
typedef uint32_t TickType_t;
typedef int BaseType_t;
typedef unsigned int UBaseType_t;
#define pdTRUE  1
#define pdFALSE 0
#define pdPASS  1
#define pdFAIL  0
#define portMAX_DELAY ((TickType_t) 0xFFFFFFFFu)
#define pdMS_TO_TICKS(ms) ((TickType_t) (ms))
#define portTICK_PERIOD_MS 1
typedef struct { int unused; } portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED { 0 }
#define portENTER_CRITICAL(m) ((void) (m))
#define portEXIT_CRITICAL(m) ((void) (m))
