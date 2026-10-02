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

// Temperatura del chip letta dal task di sorveglianza ogni 5 s (unico a
// usare il sensore), con il massimo dall'accensione. -1000 = non ancora letta.
static volatile float s_temp_now = -1000;
static volatile float s_temp_max = -1000;
static volatile int64_t s_temp_max_us; // quando e' stata raggiunta (esp_timer)

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
        // ulRunTimeCounter e' a 32 bit in microsecondi: torna a zero ogni
        // ~71 minuti. Se dall'ultima lettura e' passato piu' di un giro (il
        // pannello e' rimasto chiuso a lungo), il delta non e' affidabile:
        // si riparte da questa lettura invece di mostrare un valore falso
        // (visto: 98% su entrambi i core dopo 5 h, 2% alla lettura dopo).
        if (wall_delta > 0 && wall_delta < 60LL * 60 * 1000000) {
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

    s.chip_temp_c = s_temp_now;
    s.chip_temp_max_c = s_temp_max;
    s.chip_temp_max_us = s_temp_max_us;

    return s;
}

// ---------------------------------------------------------------------------
// Sorveglianza in background: ogni 5 s controlla uso CPU e memoria e, solo
// se qualcosa esce dalla norma, scrive nel log quali task stavano lavorando.
// Aggiunta dopo due episodi notturni senza spiegazione (core 0 al 100% per
// alcuni minuti, memoria libera minima scesa a 10 KB): il pannello mostra
// solo il valore, non la causa.
// ---------------------------------------------------------------------------

#include "esp_log.h"
#include <stdio.h>
#include <string.h>

#define MON_PERIOD_MS       5000
#define MON_MAX_TASKS       40
#define MON_CPU_ALERT_PCT   80.0f
#define MON_HEAP_ALERT      16384
#define MON_LOG_GAP_US      (60LL * 1000000) // al massimo un avviso CPU al minuto

static const char *MON_TAG = "sys_mon";

typedef struct {
    TaskHandle_t handle;
    uint32_t runtime;
} mon_prev_t;

// Statici: la sorveglianza deve funzionare proprio quando la memoria scarseggia.
static TaskStatus_t s_mon_tasks[MON_MAX_TASKS];
static mon_prev_t s_mon_prev[MON_MAX_TASKS];
static UBaseType_t s_mon_prev_n;

static uint32_t prev_runtime(TaskHandle_t h, bool *found)
{
    for (UBaseType_t i = 0; i < s_mon_prev_n; i++) {
        if (s_mon_prev[i].handle == h) {
            *found = true;
            return s_mon_prev[i].runtime;
        }
    }
    *found = false;
    return 0;
}

// Scrive in out i 4 task (esclusi gli IDLE) che hanno usato piu' CPU
// nell'intervallo, in % di un core.
static void top_tasks(const uint32_t *delta, UBaseType_t n, int64_t wall_us,
                      TaskHandle_t idle0, TaskHandle_t idle1, char *out, size_t out_size)
{
    bool used[MON_MAX_TASKS] = {0};
    size_t pos = 0;
    out[0] = '\0';
    for (int k = 0; k < 4; k++) {
        int best = -1;
        for (UBaseType_t i = 0; i < n; i++) {
            if (used[i] || s_mon_tasks[i].xHandle == idle0 || s_mon_tasks[i].xHandle == idle1) {
                continue;
            }
            if (best < 0 || delta[i] > delta[best]) {
                best = (int) i;
            }
        }
        if (best < 0 || delta[best] == 0) {
            break;
        }
        used[best] = true;
        int w = snprintf(out + pos, out_size - pos, "%s%s %.0f%%", pos ? ", " : "",
                         s_mon_tasks[best].pcTaskName, (double) delta[best] * 100.0 / (double) wall_us);
        if (w < 0 || (size_t) w >= out_size - pos) {
            break;
        }
        pos += (size_t) w;
    }
}

static void safety_restart_check(int64_t now);

static void sys_monitor_task(void *arg)
{
    int64_t prev_wall = esp_timer_get_time();
    int64_t last_cpu_log = -MON_LOG_GAP_US;
    uint32_t logged_min_heap = UINT32_MAX;
    static uint32_t delta[MON_MAX_TASKS];
    char top[160];
    bool temp_high = false;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(MON_PERIOD_MS));

        float t = read_chip_temp_c();
        if (t > -1000) {
            s_temp_now = t;
            if (t > s_temp_max) {
                s_temp_max = t;
                s_temp_max_us = esp_timer_get_time();
            }
            if (!temp_high && t >= SYS_TEMP_ALERT_C) {
                temp_high = true;
                ESP_LOGW(MON_TAG, "Temperatura del chip alta: %.1f C (soglia %.0f C). Controlla sole diretto o ventilazione della custodia", (double) t, (double) SYS_TEMP_ALERT_C);
            } else if (temp_high && t < SYS_TEMP_REARM_C) {
                temp_high = false;
                ESP_LOGI(MON_TAG, "Temperatura del chip rientrata: %.1f C (massima %.1f C)", (double) t, (double) s_temp_max);
            }
        }

        UBaseType_t n = uxTaskGetSystemState(s_mon_tasks, MON_MAX_TASKS, NULL);
        int64_t now = esp_timer_get_time();
        int64_t wall = now - prev_wall;
        prev_wall = now;
        safety_restart_check(now);
        if (n == 0 || wall <= 0) {
            continue; // array troppo piccolo: non dovrebbe succedere
        }

        bool have_prev = s_mon_prev_n > 0;
        for (UBaseType_t i = 0; i < n; i++) {
            bool found;
            uint32_t p = prev_runtime(s_mon_tasks[i].xHandle, &found);
            delta[i] = found ? s_mon_tasks[i].ulRunTimeCounter - p : 0;
        }
        for (UBaseType_t i = 0; i < n; i++) {
            s_mon_prev[i].handle = s_mon_tasks[i].xHandle;
            s_mon_prev[i].runtime = s_mon_tasks[i].ulRunTimeCounter;
        }
        s_mon_prev_n = n;
        if (!have_prev) {
            continue;
        }

        TaskHandle_t idle0 = xTaskGetIdleTaskHandleForCore(0);
        TaskHandle_t idle1 = xTaskGetIdleTaskHandleForCore(1);
        float busy[2] = { -1, -1 };
        for (UBaseType_t i = 0; i < n; i++) {
            int core = s_mon_tasks[i].xHandle == idle0 ? 0 : (s_mon_tasks[i].xHandle == idle1 ? 1 : -1);
            if (core >= 0) {
                float idle_pct = (float) delta[i] * 100.0f / (float) wall;
                busy[core] = idle_pct > 100 ? 0 : 100.0f - idle_pct;
            }
        }

        if ((busy[0] >= MON_CPU_ALERT_PCT || busy[1] >= MON_CPU_ALERT_PCT) &&
            now - last_cpu_log >= MON_LOG_GAP_US) {
            last_cpu_log = now;
            top_tasks(delta, n, wall, idle0, idle1, top, sizeof(top));
            ESP_LOGW(MON_TAG, "CPU alta negli ultimi %lld s: core0 %.0f%%, core1 %.0f%%. Task piu' attivi: %s",
                     wall / 1000000, (double) busy[0], (double) busy[1], top);
        }

        uint32_t min_heap = esp_get_minimum_free_heap_size();
        if (min_heap < MON_HEAP_ALERT && min_heap < logged_min_heap) {
            logged_min_heap = min_heap;
            top_tasks(delta, n, wall, idle0, idle1, top, sizeof(top));
            ESP_LOGW(MON_TAG, "Memoria libera minima scesa a %u byte negli ultimi %lld s (ora libera %u, blocco piu' grande %u). Task piu' attivi: %s",
                     (unsigned) min_heap, wall / 1000000, (unsigned) esp_get_free_heap_size(),
                     (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_8BIT), top);
        }
    }
}

void sys_stats_monitor_start(void)
{
    xTaskCreate(sys_monitor_task, "sys_mon", 3584, NULL, 1, NULL);
}

// ---------------------------------------------------------------------------
// Motivo dell'ultimo riavvio e riavvio di sicurezza.
// Il motivo scelto dal firmware sopravvive al riavvio in memoria RTC (non
// azzerata da un riavvio software, persa solo togliendo corrente); quello
// hardware lo da' il chip (accensione, calo di tensione, crash, watchdog).
// ---------------------------------------------------------------------------

#include "esp_attr.h"
#include "status.h"
#include "gnss_fix.h"

#define RESTART_MAGIC 0x52535452u

typedef struct {
    uint32_t magic;
    char reason[96];
} restart_note_t;

static RTC_NOINIT_ATTR restart_note_t s_restart_note;
static char s_last_reset[160];

void sys_stats_restart_with_reason(const char *reason)
{
    s_restart_note.magic = RESTART_MAGIC;
    strncpy(s_restart_note.reason, reason, sizeof(s_restart_note.reason) - 1);
    s_restart_note.reason[sizeof(s_restart_note.reason) - 1] = 0;
    ESP_LOGW(MON_TAG, "Riavvio: %s", reason);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void sys_stats_note_restart_reason(const char *reason)
{
    s_restart_note.magic = RESTART_MAGIC;
    strncpy(s_restart_note.reason, reason, sizeof(s_restart_note.reason) - 1);
    s_restart_note.reason[sizeof(s_restart_note.reason) - 1] = 0;
}

void sys_stats_boot_report(void)
{
    const char *hw;
    switch (esp_reset_reason()) {
    case ESP_RST_POWERON:  hw = "accensione (alimentazione collegata)"; break;
    case ESP_RST_SW:       hw = "riavvio software"; break;
    case ESP_RST_PANIC:    hw = "errore del firmware (crash)"; break;
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:      hw = "blocco del firmware (watchdog)"; break;
    case ESP_RST_BROWNOUT: hw = "calo di tensione dell'alimentazione (alimentatore debole?)"; break;
    case ESP_RST_EXT:      hw = "pulsante di reset"; break;
    default:               hw = "motivo sconosciuto"; break;
    }
    bool have_note = (esp_reset_reason() == ESP_RST_SW && s_restart_note.magic == RESTART_MAGIC);
    s_restart_note.reason[sizeof(s_restart_note.reason) - 1] = 0;
    snprintf(s_last_reset, sizeof(s_last_reset), "%s%s%s", hw,
             have_note ? ": " : "", have_note ? s_restart_note.reason : "");
    s_restart_note.magic = 0;
    if (esp_reset_reason() == ESP_RST_PANIC || esp_reset_reason() == ESP_RST_BROWNOUT ||
        esp_reset_reason() == ESP_RST_INT_WDT || esp_reset_reason() == ESP_RST_TASK_WDT ||
        esp_reset_reason() == ESP_RST_WDT) {
        ESP_LOGW(MON_TAG, "Motivo dell'ultimo riavvio: %s", s_last_reset);
    } else {
        ESP_LOGI(MON_TAG, "Motivo dell'ultimo riavvio: %s", s_last_reset);
    }
}

const char *sys_stats_last_reset(void)
{
    return s_last_reset;
}

// Riavvio di sicurezza, chiamato ogni 5 s dal task di sorveglianza. Solo per
// blocchi dopo che le cose hanno funzionato almeno una volta dall'avvio:
// una base senza ricevitore collegato (prove) non si riavvia di continuo.
#define GUARD_GNSS_SILENT_US   (10LL * 60 * 1000000)
#define GUARD_CASTER_DOWN_US   (30LL * 60 * 1000000)

static void safety_restart_check(int64_t now)
{
    char why[96];
    if (!status_get_active_rover()) {
        int64_t last = status_get_last_rtcm_time_us();
        if (last > 0 && now - last > GUARD_GNSS_SILENT_US) {
            sys_stats_restart_with_reason("nessun dato RTCM dal ricevitore da 10 minuti");
        }
    } else {
        gnss_fix_status_t fx = gnss_fix_get_status();
        if (fx.valid && now - fx.last_update_us > GUARD_GNSS_SILENT_US) {
            sys_stats_restart_with_reason("nessun dato NMEA dal ricevitore da 10 minuti");
        }
    }
    ntrip_conn_status_t nt = status_ntrip_get();
    if (nt.connect_count > 0 && !nt.connected && nt.last_disconnect_us > 0 &&
        now - nt.last_disconnect_us > GUARD_CASTER_DOWN_US) {
        snprintf(why, sizeof(why), "caster scollegato da 30 minuti (%.50s)", nt.last_error);
        sys_stats_restart_with_reason(why);
    }
}
