#include "sys_stats.h"

#include "esp_heap_caps.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "driver/temperature_sensor.h"

typedef struct {
    bool have_baseline;
    int64_t wall_us;
    uint32_t idle_us;
} core_sample_t;

static core_sample_t s_prev[2];

// Percentuale di uso CPU per un core, calcolata come 100% meno la
// percentuale di tempo passata dal suo task IDLE nell'intervallo trascorso
// dall'ultimo campione. La sottrazione tra due valori uint32_t e'
// corretta anche se ulRunTimeCounter ha fatto un giro (wraparound) tra un
// campione e l'altro, finche' l'intervallo reale tra le chiamate resta
// sotto il periodo di overflow del contatore (~71 minuti a 1 campione al
// secondo, larghissimo margine rispetto al polling della UI web ogni
// pochi secondi).
static float compute_cpu_percent(int core_idx, TaskHandle_t idle_handle, int64_t now_wall_us)
{
    if (!idle_handle) {
        return -1;
    }

    TaskStatus_t status;
    vTaskGetInfo(idle_handle, &status, pdFALSE, eInvalid);
    uint32_t idle_us = (uint32_t) status.ulRunTimeCounter;

    core_sample_t *prev = &s_prev[core_idx];
    float result = -1;
    if (prev->have_baseline) {
        uint32_t idle_delta = idle_us - prev->idle_us;
        int64_t wall_delta = now_wall_us - prev->wall_us;
        if (wall_delta > 0) {
            float idle_pct = ((float) idle_delta / (float) wall_delta) * 100.0f;
            if (idle_pct < 0) idle_pct = 0;
            if (idle_pct > 100) idle_pct = 100;
            result = 100.0f - idle_pct;
        }
    }

    prev->have_baseline = true;
    prev->wall_us = now_wall_us;
    prev->idle_us = idle_us;
    return result;
}

static temperature_sensor_handle_t s_tsens;
static bool s_tsens_init_failed;

// Installa/abilita il sensore di temperatura interno al primo utilizzo -
// intervallo 20-100 C (precisione dichiarata +-2 C), un buon compromesso
// per un dispositivo che puo' scaldarsi in una custodia chiusa al sole,
// senza restringere troppo il range per un chip che in condizioni normali
// lavora gia' oltre la temperatura ambiente.
static float read_chip_temp_c(void)
{
    if (s_tsens_init_failed) {
        return -1000;
    }
    if (!s_tsens) {
        temperature_sensor_config_t config = TEMPERATURE_SENSOR_CONFIG_DEFAULT(20, 100);
        if (temperature_sensor_install(&config, &s_tsens) != ESP_OK ||
            temperature_sensor_enable(s_tsens) != ESP_OK) {
            s_tsens_init_failed = true;
            return -1000;
        }
    }
    float celsius = -1000;
    if (temperature_sensor_get_celsius(s_tsens, &celsius) != ESP_OK) {
        return -1000;
    }
    return celsius;
}

sys_stats_t sys_stats_get(void)
{
    sys_stats_t s = {0};

    s.free_heap_bytes = esp_get_free_heap_size();
    s.min_free_heap_bytes = esp_get_minimum_free_heap_size();
    s.total_heap_bytes = heap_caps_get_total_size(MALLOC_CAP_INTERNAL);

    size_t psram_total = heap_caps_get_total_size(MALLOC_CAP_SPIRAM);
    s.psram_present = psram_total > 0;
    if (s.psram_present) {
        s.total_psram_bytes = psram_total;
        s.free_psram_bytes = heap_caps_get_free_size(MALLOC_CAP_SPIRAM);
    }

    int64_t now = esp_timer_get_time();
    s.cpu0_percent = compute_cpu_percent(0, xTaskGetIdleTaskHandleForCore(0), now);
    s.cpu1_percent = compute_cpu_percent(1, xTaskGetIdleTaskHandleForCore(1), now);

    s.chip_temp_c = read_chip_temp_c();

    return s;
}
