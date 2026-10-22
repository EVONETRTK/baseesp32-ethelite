#include "base_monitor.h"
#include "rtcm3_1005.h"

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static SemaphoreHandle_t s_mutex;
static bool s_last_position_set;
static rtcm3_position_t s_last_position;
static int64_t s_last_position_us;

static void mutex_init(void)
{
    if (!s_mutex) {
        s_mutex = xSemaphoreCreateMutex();
    }
}

void base_monitor_feed(const uint8_t *data, size_t len)
{
    rtcm3_position_t pos;
    if (!rtcm3_1005_feed(data, len, &pos)) {
        return;
    }

    mutex_init();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_last_position = pos;
    s_last_position_set = true;
    s_last_position_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

base_monitor_status_t base_monitor_get_status(void)
{
    base_monitor_status_t st = {0};
    if (!s_mutex) {
        return st;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    st.last_position_set = s_last_position_set;
    st.last_ecef_x_m = s_last_position.ecef_x_m;
    st.last_ecef_y_m = s_last_position.ecef_y_m;
    st.last_ecef_z_m = s_last_position.ecef_z_m;
    st.last_position_us = s_last_position_us;
    xSemaphoreGive(s_mutex);
    return st;
}
