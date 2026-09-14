#include "base_monitor.h"
#include "rtcm3_1005.h"

#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "base_monitor";

static SemaphoreHandle_t s_mutex;
static bool s_baseline_set;
static rtcm3_position_t s_baseline;
static double s_drift_m;

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
    if (!s_baseline_set) {
        s_baseline = pos;
        s_baseline_set = true;
        s_drift_m = 0;
        xSemaphoreGive(s_mutex);
        ESP_LOGI(TAG, "Posizione di riferimento registrata (ECEF X=%.4f Y=%.4f Z=%.4f)",
                 pos.ecef_x_m, pos.ecef_y_m, pos.ecef_z_m);
        return;
    }

    double dx = pos.ecef_x_m - s_baseline.ecef_x_m;
    double dy = pos.ecef_y_m - s_baseline.ecef_y_m;
    double dz = pos.ecef_z_m - s_baseline.ecef_z_m;
    s_drift_m = sqrt(dx * dx + dy * dy + dz * dz);
    xSemaphoreGive(s_mutex);
}

base_monitor_status_t base_monitor_get_status(void)
{
    base_monitor_status_t st = {0};
    if (!s_mutex) {
        return st;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    st.baseline_set = s_baseline_set;
    st.drift_m = s_drift_m;
    xSemaphoreGive(s_mutex);
    return st;
}
