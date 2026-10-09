// Stub di freertos/idf_additions.h.
#pragma once
#include "freertos/task.h"
BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h, uint32_t caps);
