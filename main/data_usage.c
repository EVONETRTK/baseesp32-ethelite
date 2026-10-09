#include "data_usage.h"
#include "status.h"
#include "sim_plan.h"
#include "eth_link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "data_usage";

#define NVS_NS            "datause"
#define SAVE_PERIOD_MS    (10 * 60 * 1000)
#define HEADER_BYTES      52   // stima per invio: IPv4 + TCP con opzioni + PPP/ACK

static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static uint32_t s_month;    // aaaamm, 0 = sconosciuto (ora non sincronizzata)
static uint32_t s_day;      // aaaammgg
static uint64_t s_cell_month, s_cell_day, s_wifi_month, s_wifi_day;
static bool s_dirty;
static uint32_t s_since; // da quando si conta nel mese (epoch s), per medie e previsioni
// Storico giornaliero (ultimi 30 giorni, dal piu' vecchio), per il grafico.
static data_usage_day_t s_hist[DATA_USAGE_HISTORY_DAYS];
static uint32_t s_hist_n;

// Aggiorna mese/giorno correnti e azzera i totali al cambio. Finche' l'ora
// non e' nota (prima dell'NTP) si continua sul periodo salvato.
static void roll_period(void)
{
    time_t now = time(NULL);
    if (now < 1700000000) {
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    uint32_t day = (uint32_t) (tm.tm_year + 1900) * 10000 + (uint32_t) (tm.tm_mon + 1) * 100 + (uint32_t) tm.tm_mday;
    // Periodo di conteggio = da un rinnovo della SIM all'altro (sim_plan.c),
    // o il mese di calendario se il rinnovo non e' impostato. s_month ne
    // contiene l'inizio (aaaammgg); un valore a 6 cifre (aaaamm) viene dalla
    // 1.19.80-81: si tengono i conteggi e si passa al nuovo formato.
    uint32_t month = sim_plan_period_start(day);
    // Inizio del periodo come epoch, calcolato FUORI dalla sezione critica:
    // mktime() prende dei lock e dentro portENTER_CRITICAL manda il firmware
    // in crash (visto sul dispositivo al primo cambio di periodo).
    struct tm first = tm;
    first.tm_year = (int) (month / 10000) - 1900;
    first.tm_mon = (int) (month / 100 % 100) - 1;
    first.tm_mday = (int) (month % 100);
    first.tm_hour = 0; first.tm_min = 0; first.tm_sec = 0; first.tm_isdst = -1;
    time_t period_epoch = mktime(&first);
    if (period_epoch > now) {
        period_epoch = now;
    }
    portENTER_CRITICAL(&s_lock);
    if (s_month >= 10000000 && month != s_month) {
        s_cell_month = 0;
        s_wifi_month = 0;
        s_since = (uint32_t) period_epoch; // dall'inizio del nuovo periodo
        s_dirty = true;
    }
    if (s_since == 0) {
        s_since = (uint32_t) now;
        s_dirty = true;
    }
    if (s_day != 0 && day != s_day) {
        // Il giorno finito va nello storico (in KB: bastano 32 bit).
        if (s_hist_n == DATA_USAGE_HISTORY_DAYS) {
            memmove(&s_hist[0], &s_hist[1], sizeof(s_hist[0]) * (DATA_USAGE_HISTORY_DAYS - 1));
            s_hist_n--;
        }
        s_hist[s_hist_n].day = s_day;
        s_hist[s_hist_n].cell_kb = (uint32_t) (s_cell_day / 1000);
        s_hist[s_hist_n].wifi_kb = (uint32_t) (s_wifi_day / 1000);
        s_hist_n++;
        s_cell_day = 0;
        s_wifi_day = 0;
    }
    if (month != s_month || day != s_day) {
        s_dirty = true;
    }
    s_month = month;
    s_day = day;
    portEXIT_CRITICAL(&s_lock);
}

void data_usage_add(uint32_t bytes, bool sent)
{
    net_status_t net = status_get_net();
    // Con la sola Ethernet (status_get_net() = NONE) il traffico prima non
    // veniva contato: va con quello della rete fissa (WiFi), non della SIM.
    if (bytes == 0 || (net == NET_STATUS_NONE && !eth_link_is_connected())) {
        return;
    }
    uint64_t b = (uint64_t) bytes + (sent ? HEADER_BYTES : 0);
    portENTER_CRITICAL(&s_lock);
    if (net == NET_STATUS_CELLULAR) {
        s_cell_month += b;
        s_cell_day += b;
    } else {
        s_wifi_month += b;
        s_wifi_day += b;
    }
    s_dirty = true;
    portEXIT_CRITICAL(&s_lock);
}

data_usage_t data_usage_get(void)
{
    data_usage_t u = {0};
    portENTER_CRITICAL(&s_lock);
    uint32_t month = s_month;
    u.cell_month = s_cell_month;
    u.cell_day = s_cell_day;
    u.wifi_month = s_wifi_month;
    u.wifi_day = s_wifi_day;
    uint32_t since = s_since;
    portEXIT_CRITICAL(&s_lock);
    time_t now = time(NULL);
    if (since && now > (time_t) since) {
        u.counted_s = (uint32_t) (now - since);
    }
    if (month >= 10000000) {
        snprintf(u.month, sizeof(u.month), "%04u-%02u-%02u", (unsigned) (month / 10000),
                 (unsigned) (month / 100 % 100), (unsigned) (month % 100));
        u.period_days = sim_plan_period_days(month);
    }
    return u;
}

static void save(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    portENTER_CRITICAL(&s_lock);
    uint32_t month = s_month, day = s_day;
    uint64_t cm = s_cell_month, cd = s_cell_day, wm = s_wifi_month, wd = s_wifi_day;
    s_dirty = false;
    portEXIT_CRITICAL(&s_lock);
    nvs_set_u32(h, "m", month);
    nvs_set_u32(h, "d", day);
    nvs_set_u32(h, "s", s_since);
    nvs_set_u64(h, "cm", cm);
    nvs_set_u64(h, "cd", cd);
    nvs_set_u64(h, "wm", wm);
    nvs_set_u64(h, "wd", wd);
    static data_usage_day_t hist[DATA_USAGE_HISTORY_DAYS];
    portENTER_CRITICAL(&s_lock);
    uint32_t hn = s_hist_n;
    memcpy(hist, s_hist, sizeof(hist));
    portEXIT_CRITICAL(&s_lock);
    nvs_set_blob(h, "hist", hist, sizeof(hist[0]) * hn);
    nvs_commit(h);
    nvs_close(h);
}

static void load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; // primo avvio: tutto a zero
    }
    nvs_get_u32(h, "m", &s_month);
    nvs_get_u32(h, "d", &s_day);
    nvs_get_u32(h, "s", &s_since);
    nvs_get_u64(h, "cm", &s_cell_month);
    nvs_get_u64(h, "cd", &s_cell_day);
    nvs_get_u64(h, "wm", &s_wifi_month);
    nvs_get_u64(h, "wd", &s_wifi_day);
    size_t len = sizeof(s_hist);
    if (nvs_get_blob(h, "hist", s_hist, &len) == ESP_OK) {
        s_hist_n = len / sizeof(s_hist[0]);
    }
    nvs_close(h);
}

// Chiamata ogni 5 s dal task di sorveglianza (sys_stats.c): nessun task
// proprio, la memoria libera e' poca.
void data_usage_tick(void)
{
    static TickType_t last_roll, last_save;
    TickType_t now = xTaskGetTickCount();
    if (now - last_roll < pdMS_TO_TICKS(30000)) {
        return;
    }
    last_roll = now;
    roll_period();
    if (s_dirty && now - last_save >= pdMS_TO_TICKS(SAVE_PERIOD_MS)) {
        save();
        last_save = now;
    }
}

void data_usage_start(void)
{
    load();
    ESP_LOGI(TAG, "Traffico del mese (stima): cellulare %.1f MB, WiFi %.1f MB",
             s_cell_month / 1e6, s_wifi_month / 1e6);
}

int data_usage_get_history(data_usage_day_t *out, int max)
{
    portENTER_CRITICAL(&s_lock);
    int n = (int) s_hist_n < max ? (int) s_hist_n : max;
    memcpy(out, &s_hist[s_hist_n - n], sizeof(out[0]) * n);
    portEXIT_CRITICAL(&s_lock);
    return n;
}
