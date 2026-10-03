#include "sys_stats.h"
#include "esp_attr.h"
#include "esp_task_wdt.h"
#include "settings.h"
#include "gnss_ubx.h"
#include "ota_update.h"
#include "data_usage.h"

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

    // Solo RAM interna: con la PSRAM attiva (1.19.115) heap_caps_get_free_size(MALLOC_CAP_INTERNAL)
    // conta anche gli 8 MB esterni e non mostrerebbe piu' la memoria che serve
    // davvero a WiFi, rete e stack. La PSRAM e' in free_psram_bytes.
    s.free_heap_bytes = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    s.min_free_heap_bytes = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
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

// Battiti dei task principali (vedi sys_stats.h): ultimo istante e limite.
static volatile int64_t s_hb_last_us[HB_COUNT];
static const struct { const char *name; int64_t limit_us; } s_hb_info[HB_COUNT] = {
    [HB_NET]    = { "rete (net_manager)",  10LL * 60 * 1000000 },
    [HB_NTRIP]  = { "client NTRIP",          5LL * 60 * 1000000 },
    [HB_GNSS]   = { "lettura del ricevitore", 2LL * 60 * 1000000 },
    [HB_ALERTS] = { "avvisi",               10LL * 60 * 1000000 },
};

static volatile uint32_t s_hb_frozen; // prova: battiti ignorati (sys_stats_test_fault)

void sys_stats_heartbeat(heartbeat_t which)
{
    if (which < HB_COUNT && !(s_hb_frozen & (1u << which))) {
        s_hb_last_us[which] = esp_timer_get_time();
    }
}

// Ultima misura (ogni 5 s) dei 3 task con meno stack libero, conservata
// anche dopo un crash: dice chi era al limite.
static RTC_NOINIT_ATTR char s_low_stacks_rtc[64];

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
        // Task bloccati senza crash (vedi sys_stats_heartbeat).
        for (int h = 0; h < HB_COUNT; h++) {
            int64_t last = s_hb_last_us[h];
            if (last > 0 && now - last > s_hb_info[h].limit_us) {
                char why[96];
                snprintf(why, sizeof(why), "task bloccato: %s, fermo da %lld s",
                         s_hb_info[h].name, (long long) ((now - last) / 1000000));
                sys_stats_restart_with_reason(why);
            }
        }
        // Watchdog hardware (vedi sys_stats_monitor_start): questo task e'
        // vivo. Se si blocca lui, o una CPU per 60 s, riavvio automatico.
        esp_task_wdt_reset();
        {
            static int64_t last_heap_log;
            if (now - last_heap_log >= 600LL * 1000000) { // ogni 10 minuti, per seguire la memoria nel tempo
                last_heap_log = now;
                ESP_LOGI(MON_TAG, "Memoria libera %u (minima %u, blocco piu' grande %u)", (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                         (unsigned) heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL), (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
            }
        }
        ota_update_confirm_tick();
        data_usage_tick();
        if (n == 0 || wall <= 0) {
            continue; // array troppo piccolo: non dovrebbe succedere
        }

        // Task con meno stack libero (a 20 s e 50 s dall'avvio, poi ogni 10
        // minuti): per trovare chi trabocca (crash rari dopo gli
        // aggiornamenti, ottobre 2026). Usa l'elenco appena letto sopra.
        {
            static int stack_reports;
            static int64_t last_stack_report;
            bool due = (stack_reports == 0 && now > 20LL * 1000000) || (stack_reports == 1 && now > 50LL * 1000000) ||
                       (stack_reports >= 2 && now - last_stack_report >= 600LL * 1000000);
            if (due) {
                stack_reports++;
                last_stack_report = now;
                char line[200];
                size_t pos = 0;
                bool used[MON_MAX_TASKS] = {0};
                for (int k = 0; k < 6; k++) {
                    int best = -1;
                    for (UBaseType_t i = 0; i < n; i++) {
                        if (!used[i] && (best < 0 || s_mon_tasks[i].usStackHighWaterMark < s_mon_tasks[best].usStackHighWaterMark)) {
                            best = (int) i;
                        }
                    }
                    if (best < 0) {
                        break;
                    }
                    used[best] = true;
                    int w = snprintf(line + pos, sizeof(line) - pos, "%s%s %u", pos ? ", " : "",
                                     s_mon_tasks[best].pcTaskName, (unsigned) s_mon_tasks[best].usStackHighWaterMark);
                    if (w < 0 || (size_t) w >= sizeof(line) - pos) {
                        break;
                    }
                    pos += (size_t) w;
                }
                ESP_LOGI(MON_TAG, "Stack libero minimo (byte): %s", line);
            }
        }
        {
            // I 3 task con meno stack libero adesso, in memoria RTC (vedi sopra).
            char tmp[64];
            size_t pos = 0;
            bool used3[MON_MAX_TASKS] = {0};
            for (int k = 0; k < 3; k++) {
                int best = -1;
                for (UBaseType_t i = 0; i < n; i++) {
                    if (!used3[i] && (best < 0 || s_mon_tasks[i].usStackHighWaterMark < s_mon_tasks[best].usStackHighWaterMark)) {
                        best = (int) i;
                    }
                }
                if (best < 0) break;
                used3[best] = true;
                int w = snprintf(tmp + pos, sizeof(tmp) - pos, "%s%.10s %u", pos ? "," : "",
                                 s_mon_tasks[best].pcTaskName, (unsigned) s_mon_tasks[best].usStackHighWaterMark);
                if (w < 0 || (size_t) w >= sizeof(tmp) - pos) break;
                pos += (size_t) w;
            }
            memcpy(s_low_stacks_rtc, tmp, sizeof(tmp));
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

        uint32_t min_heap = heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL);
        if (min_heap < MON_HEAP_ALERT && min_heap < logged_min_heap) {
            logged_min_heap = min_heap;
            top_tasks(delta, n, wall, idle0, idle1, top, sizeof(top));
            ESP_LOGW(MON_TAG, "Memoria libera minima scesa a %u byte negli ultimi %lld s (ora libera %u, blocco piu' grande %u). Task piu' attivi: %s",
                     (unsigned) min_heap, wall / 1000000, (unsigned) heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
                     (unsigned) heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL), top);
        }
    }
}

void sys_stats_monitor_start(void)
{
    // Watchdog dei task di ESP-IDF: prima era a 5 s e solo segnalava (nessun
    // riavvio). Ora 60 s con riavvio: controlla i due core (task inattivi) e
    // il task di sorveglianza, che a sua volta controlla i battiti degli altri.
    const esp_task_wdt_config_t twdt = {
        .timeout_ms = 60000,
        .idle_core_mask = (1 << 0) | (1 << 1),
        .trigger_panic = true,
    };
    esp_task_wdt_reconfigure(&twdt);
    TaskHandle_t h = NULL;
    xTaskCreate(sys_monitor_task, "sys_mon", 4608, NULL, 1, &h);
    if (h) {
        esp_task_wdt_add(h);
    } // 4,5 KB: con elenco dei task e misure dello stack ne restavano meno di 1
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

// Riavvio di sicurezza in corso (sys_stats_restart_with_reason): al prossimo
// avvio conta come guasto per la protezione dai riavvii a catena
// (config_backup_crash_guard), come un crash.
#define FAULT_MAGIC 0x46415554u
static RTC_NOINIT_ATTR uint32_t s_fault_restart;
static bool s_boot_fault;

bool sys_stats_boot_was_fault(void)
{
    return s_boot_fault;
}

void sys_stats_restart_with_reason(const char *reason)
{
    s_restart_note.magic = RESTART_MAGIC;
    strncpy(s_restart_note.reason, reason, sizeof(s_restart_note.reason) - 1);
    s_restart_note.reason[sizeof(s_restart_note.reason) - 1] = 0;
    s_fault_restart = FAULT_MAGIC;
    ESP_LOGW(MON_TAG, "Riavvio: %s", reason);
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
}

void sys_stats_note_restart_reason(const char *reason)
{
    // Riavvio voluto (pannello, aggiornamento, misura...): se il firmware e'
    // ancora in prova va confermato, altrimenti il bootloader lo annullerebbe.
    ota_update_mark_valid();
    s_fault_restart = 0;
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
    esp_reset_reason_t rr = esp_reset_reason();
    s_boot_fault = rr == ESP_RST_PANIC || rr == ESP_RST_INT_WDT || rr == ESP_RST_TASK_WDT ||
                   rr == ESP_RST_WDT || (rr == ESP_RST_SW && s_fault_restart == FAULT_MAGIC);
    s_fault_restart = 0;
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

static void peek_chip(const app_settings_t *s, void *ctx)
{
    *(gnss_chip_t *) ctx = s->gnss_chip;
}

// Ricevitore muto: se e' un u-blox lo si riavvia insieme alla base (ha
// alimentazione propria, il riavvio dell'ESP32 da solo non lo sbloccherebbe).
static void receiver_silent_restart(const char *why)
{
    gnss_chip_t chip = GNSS_CHIP_UBLOX;
    settings_peek(peek_chip, &chip);
    if (chip == GNSS_CHIP_UBLOX) {
        gnss_ubx_hw_reset();
    }
    sys_stats_restart_with_reason(why);
}

static void test_hang_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000)); // tempo per rispondere alla richiesta
    ESP_LOGW(MON_TAG, "PROVA: core 0 bloccato, il watchdog deve riavviare entro 60 s");
    for (;;) {
    }
}

static void test_panic_task(void *arg)
{
    vTaskDelay(pdMS_TO_TICKS(1000));
    ESP_LOGW(MON_TAG, "PROVA: crash volontario");
    abort();
}

bool sys_stats_test_fault(const char *what)
{
    static const char *const freeze[HB_COUNT] = {
        [HB_NET] = "freeze_net", [HB_NTRIP] = "freeze_ntrip",
        [HB_GNSS] = "freeze_gnss", [HB_ALERTS] = "freeze_alerts",
    };
    for (int h = 0; h < HB_COUNT; h++) {
        if (strcmp(what, freeze[h]) == 0) {
            ESP_LOGW(MON_TAG, "PROVA: battito di \"%s\" ignorato, la base deve riavviarsi entro %lld s",
                     s_hb_info[h].name, (long long) (s_hb_info[h].limit_us / 1000000));
            s_hb_frozen |= 1u << h;
            return true;
        }
    }
    if (strcmp(what, "hang") == 0) {
        return xTaskCreatePinnedToCore(test_hang_task, "test_hang", 4096, NULL, 1, NULL, 0) == pdPASS;
    }
    if (strcmp(what, "panic") == 0) {
        return xTaskCreate(test_panic_task, "test_panic", 4096, NULL, 5, NULL) == pdPASS;
    }
    if (strcmp(what, "gnss_reset") == 0) {
        receiver_silent_restart("PROVA: riavvio del ricevitore e della base");
        return true; // non ci arriva
    }
    return false;
}

static void safety_restart_check(int64_t now)
{
    char why[96];
    if (!status_get_active_rover()) {
        int64_t last = status_get_last_rtcm_time_us();
        if (last > 0 && now - last > GUARD_GNSS_SILENT_US) {
            receiver_silent_restart("nessun dato RTCM dal ricevitore da 10 minuti");
        }
    } else {
        gnss_fix_status_t fx = gnss_fix_get_status();
        if (fx.valid && now - fx.last_update_us > GUARD_GNSS_SILENT_US) {
            receiver_silent_restart("nessun dato NMEA dal ricevitore da 10 minuti");
        }
    }
    ntrip_conn_status_t nt = status_ntrip_get();
    if (nt.connect_count > 0 && !nt.connected && nt.last_disconnect_us > 0 &&
        now - nt.last_disconnect_us > GUARD_CASTER_DOWN_US) {
        snprintf(why, sizeof(why), "caster scollegato da 30 minuti (%.50s)", nt.last_error);
        sys_stats_restart_with_reason(why);
    }
}

// ---------------------------------------------------------------------------
// Dove e' avvenuto l'ultimo crash. Il 03/10/2026 la 1.19.86 e' andata in crash
// una volta, 2,5 minuti dopo un aggiornamento, e non si e' piu' ripetuto:
// senza cavo USB non si sapeva dove. Il gestore dei crash di ESP-IDF viene
// "avvolto" (opzione --wrap del linker, vedi CMakeLists.txt): prima di
// lasciarlo proseguire salva motivo e indirizzi in memoria RTC, che
// sopravvive al riavvio. All'avvio successivo finiscono nel log e nel
// pannello; con il file .elf della stessa versione si risale alla riga.
// ---------------------------------------------------------------------------

#include "esp_private/panic_internal.h"
#include "xtensa_context.h"

#define CRASH_MAGIC 0x43525348u

typedef struct {
    uint32_t magic;
    uint32_t pc;        // istruzione del crash
    uint32_t caller;    // indirizzo di ritorno (a0): chi ha chiamato
    int core;
    char reason[48];
    char details[64];   // per abort(): "abort() was called at PC ..."
    char descr[32];     // task in esecuzione al momento del crash
} crash_note_t;

static RTC_NOINIT_ATTR crash_note_t s_crash_note;
static char s_low_stacks_prev[64];
static char s_crash_text[260];

extern char *g_panic_abort_details;
extern void __real_esp_panic_handler(panic_info_t *info);

static IRAM_ATTR void copy_str(char *dst, size_t n, const char *src)
{
    size_t i = 0;
    if (src) {
        for (; i < n - 1 && src[i]; i++) {
            dst[i] = src[i];
        }
    }
    dst[i] = 0;
}

IRAM_ATTR void __wrap_esp_panic_handler(panic_info_t *info)
{
    // Solo copie semplici: siamo nel gestore del crash.
    s_crash_note.magic = CRASH_MAGIC;
    s_crash_note.pc = (uint32_t) info->addr;
    s_crash_note.caller = info->frame ? (uint32_t) ((const XtExcFrame *) info->frame)->a0 : 0;
    s_crash_note.core = info->core;
    copy_str(s_crash_note.reason, sizeof(s_crash_note.reason), info->reason);
    copy_str(s_crash_note.details, sizeof(s_crash_note.details), g_panic_abort_details);
    // Nome del task in esecuzione (per un watchpoint di fine stack e' il task
    // che e' traboccato), come fa il gestore di ESP-IDF.
    TaskHandle_t cur = xTaskGetCurrentTaskHandleForCore(info->core);
    copy_str(s_crash_note.descr, sizeof(s_crash_note.descr), cur ? pcTaskGetName(cur) : "?");
    __real_esp_panic_handler(info);
}

void sys_stats_crash_report(void)
{
    if (s_crash_note.magic == CRASH_MAGIC && (esp_reset_reason() == ESP_RST_PANIC ||
                                         esp_reset_reason() == ESP_RST_TASK_WDT ||
                                         esp_reset_reason() == ESP_RST_INT_WDT)) {
        s_crash_note.reason[sizeof(s_crash_note.reason) - 1] = 0;
        s_crash_note.details[sizeof(s_crash_note.details) - 1] = 0;
        s_crash_note.descr[sizeof(s_crash_note.descr) - 1] = 0;
        s_low_stacks_rtc[sizeof(s_low_stacks_rtc) - 1] = 0;
        memcpy(s_low_stacks_prev, s_low_stacks_rtc, sizeof(s_low_stacks_prev));
        // Il chiamante e' salvato senza i 2 bit alti dell'indirizzo (convenzione
        // xtensa): si rimettono a 0x40 per poterlo cercare nel file .elf.
        uint32_t caller = s_crash_note.caller ? ((s_crash_note.caller & 0x3FFFFFFF) | 0x40000000) : 0;
        snprintf(s_crash_text, sizeof(s_crash_text), "%s%s%s, core %d, PC 0x%08lx, chiamato da 0x%08lx%s%s",
                 s_crash_note.reason, s_crash_note.descr[0] ? ", task " : "", s_crash_note.descr,
                 s_crash_note.core, (unsigned long) s_crash_note.pc, (unsigned long) caller,
                 s_crash_note.details[0] ? " - " : "", s_crash_note.details);
        if (s_low_stacks_prev[0]) {
            size_t l = strlen(s_crash_text);
            snprintf(s_crash_text + l, sizeof(s_crash_text) - l, " | stack piu' bassi: %s", s_low_stacks_prev);
        }
        ESP_LOGE(MON_TAG, "Ultimo crash: %s", s_crash_text);
    }
    s_crash_note.magic = 0;
}

const char *sys_stats_last_crash(void)
{
    return s_crash_text;
}
