#include "status.h"
#include "esp_timer.h"

static volatile net_status_t s_net = NET_STATUS_NONE;
static volatile uint32_t s_rtcm_bytes = 0;
static volatile int64_t s_last_rtcm_us = 0;

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
