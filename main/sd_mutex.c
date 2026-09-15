#include "sd_mutex.h"

static SemaphoreHandle_t s_sd_mutex;

void sd_mutex_init(void)
{
    s_sd_mutex = xSemaphoreCreateRecursiveMutex();
}

void sd_mutex_take(void)
{
    xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
}

void sd_mutex_give(void)
{
    xSemaphoreGiveRecursive(s_sd_mutex);
}
