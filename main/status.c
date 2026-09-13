#include "status.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static volatile net_status_t s_net = NET_STATUS_NONE;
static volatile uint32_t s_rtcm_bytes = 0;
static volatile int64_t s_last_rtcm_us = 0;

static ntrip_conn_status_t s_ntrip;
static SemaphoreHandle_t s_ntrip_mutex;

static void ntrip_mutex_init(void)
{
    if (!s_ntrip_mutex) {
        s_ntrip_mutex = xSemaphoreCreateMutex();
    }
}

void status_set_net(net_status_t s)
{
    s_net = s;
}

net_status_t status_get_net(void)
{
    return s_net;
}

void status_note_rtcm_bytes(uint32_t n)
{
    s_rtcm_bytes += n;
    s_last_rtcm_us = esp_timer_get_time();
}

uint32_t status_get_rtcm_total_bytes(void)
{
    return s_rtcm_bytes;
}

int64_t status_get_last_rtcm_time_us(void)
{
    return s_last_rtcm_us;
}

void status_ntrip_note_connected(void)
{
    ntrip_mutex_init();
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    s_ntrip.connected = true;
    s_ntrip.connected_since_us = esp_timer_get_time();
    s_ntrip.connect_count++;
    s_ntrip.last_error[0] = '\0';
    xSemaphoreGive(s_ntrip_mutex);
}

void status_ntrip_note_disconnected(const char *reason)
{
    ntrip_mutex_init();
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    s_ntrip.connected = false;
    s_ntrip.connected_since_us = 0;
    s_ntrip.last_disconnect_us = esp_timer_get_time();
    if (reason) {
        strncpy(s_ntrip.last_error, reason, sizeof(s_ntrip.last_error) - 1);
        s_ntrip.last_error[sizeof(s_ntrip.last_error) - 1] = '\0';
    }
    xSemaphoreGive(s_ntrip_mutex);
}

ntrip_conn_status_t status_ntrip_get(void)
{
    if (!s_ntrip_mutex) {
        return (ntrip_conn_status_t){0};
    }
    ntrip_conn_status_t copy;
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    copy = s_ntrip;
    xSemaphoreGive(s_ntrip_mutex);
    return copy;
}
