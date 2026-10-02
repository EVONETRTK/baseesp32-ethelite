#include "sim_plan.h"
#include "settings.h"
#include "alerts.h"
#include "data_usage.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sim_plan";

#define NVS_NS        "simplan"
#define NOTICE_HOUR   9     // i promemoria partono dalle 9 in poi

// Copia dei soli campi del rinnovo, aggiornata dal task di questo modulo
// (stack grande) e all'avvio. Prima ogni chiamata copiava tutte le
// impostazioni (~2 KB) sullo stack del chiamante: il task del contatore
// (3 KB) traboccava e la base andava in crash (visto sul dispositivo).
typedef struct {
    uint32_t date;
    uint8_t mode;
    uint16_t every;
} renew_cfg_t;

static renew_cfg_t s_rc;
static portMUX_TYPE s_rc_lock = portMUX_INITIALIZER_UNLOCKED;

static renew_cfg_t rc_get(void)
{
    portENTER_CRITICAL(&s_rc_lock);
    renew_cfg_t c = s_rc;
    portEXIT_CRITICAL(&s_rc_lock);
    return c;
}

static void rc_set(const app_settings_t *s)
{
    portENTER_CRITICAL(&s_rc_lock);
    s_rc.date = s->sim_renew_date;
    s_rc.mode = s->sim_renew_mode;
    s_rc.every = s->sim_renew_every_days;
    portEXIT_CRITICAL(&s_rc_lock);
}

// --- date come aaaammgg ----------------------------------------------------

static time_t ymd_to_time(uint32_t ymd)
{
    struct tm tm = {0};
    tm.tm_year = (int) (ymd / 10000) - 1900;
    tm.tm_mon = (int) (ymd / 100 % 100) - 1;
    tm.tm_mday = (int) (ymd % 100);
    tm.tm_hour = 12; // mezzogiorno: niente sorprese con l'ora legale
    tm.tm_isdst = -1;
    return mktime(&tm);
}

static uint32_t time_to_ymd(time_t t)
{
    struct tm tm;
    localtime_r(&t, &tm);
    return (uint32_t) (tm.tm_year + 1900) * 10000 + (uint32_t) (tm.tm_mon + 1) * 100 + (uint32_t) tm.tm_mday;
}

static int days_between(uint32_t from, uint32_t to)
{
    double d = difftime(ymd_to_time(to), ymd_to_time(from)) / 86400.0;
    return (int) (d < 0 ? d - 0.5 : d + 0.5);
}

static uint32_t add_days(uint32_t ymd, int n)
{
    return time_to_ymd(ymd_to_time(ymd) + (time_t) n * 86400);
}

static int days_in_month(int year, int month)
{
    static const int dm[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 };
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    return month == 2 && leap ? 29 : dm[month - 1];
}

// Stesso giorno n mesi dopo (o prima), ridotto all'ultimo del mese se serve
// (rinnovo il 31: a febbraio diventa il 28 o 29).
static uint32_t add_months(uint32_t ymd, int n, int wanted_day)
{
    int y = (int) (ymd / 10000), m = (int) (ymd / 100 % 100);
    int idx = y * 12 + (m - 1) + n;
    y = idx / 12;
    m = idx % 12 + 1;
    int d = wanted_day > days_in_month(y, m) ? days_in_month(y, m) : wanted_day;
    return (uint32_t) y * 10000 + (uint32_t) m * 100 + (uint32_t) d;
}

static uint32_t today_ymd(void)
{
    time_t now = time(NULL);
    return now < 1700000000 ? 0 : time_to_ymd(now);
}

// --- periodo di conteggio --------------------------------------------------

uint32_t sim_plan_period_start(uint32_t today)
{
    renew_cfg_t c = rc_get();
    uint32_t r = c.date;
    uint8_t mode = c.mode;
    int every = c.every;
    if (r && mode == SIM_RENEW_MONTHLY) {
        int day = (int) (r % 100);
        uint32_t this_month = add_months(today, 0, day);
        return this_month <= today ? this_month : add_months(today, -1, day);
    }
    if (r && mode == SIM_RENEW_DAYS && every > 0) {
        int diff = days_between(r, today); // positivo se oggi e' dopo il rinnovo
        int k = diff >= 0 ? diff / every : -((-diff + every - 1) / every);
        return add_days(r, k * every);
    }
    return today / 100 * 100 + 1; // mese di calendario
}

uint32_t sim_plan_period_days(uint32_t start)
{
    renew_cfg_t c = rc_get();
    uint32_t r = c.date;
    uint8_t mode = c.mode;
    int every = c.every;
    if (r && mode == SIM_RENEW_MONTHLY) {
        return (uint32_t) days_between(start, add_months(start, 1, (int) (r % 100)));
    }
    if (r && mode == SIM_RENEW_DAYS && every > 0) {
        return (uint32_t) every;
    }
    return (uint32_t) days_in_month((int) (start / 10000), (int) (start / 100 % 100));
}

bool sim_plan_days_left(int *days_left)
{
    uint32_t r = rc_get().date;
    uint32_t today = today_ymd();
    if (!r || !today) {
        return false;
    }
    *days_left = days_between(today, r);
    return true;
}

// --- promemoria ------------------------------------------------------------

static void send_notice(const app_settings_t *s, int days_left)
{
    data_usage_t du = data_usage_get();
    char when[24], used[80] = "";
    if (days_left > 1) {
        snprintf(when, sizeof(when), "tra %d giorni", days_left);
    } else if (days_left == 1) {
        snprintf(when, sizeof(when), "domani");
    } else if (days_left == 0) {
        snprintf(when, sizeof(when), "oggi");
    } else {
        snprintf(when, sizeof(when), "da %d giorni", -days_left);
    }
    if (s->data_plan_mb) {
        snprintf(used, sizeof(used), " Traffico del periodo: %.2f GB su %.1f GB.",
                 du.cell_month / 1e9, s->data_plan_mb / 1000.0);
    }
    char body[256];
    snprintf(body, sizeof(body), "EVONETRTK %s: %s %s (%02u/%02u/%04u). %s%s",
             s->device_serial,
             days_left < 0 ? "piano della SIM scaduto" : "rinnovo del piano della SIM",
             when, (unsigned) (s->sim_renew_date % 100), (unsigned) (s->sim_renew_date / 100 % 100),
             (unsigned) (s->sim_renew_date / 10000),
             days_left < 0 ? "Rinnova il piano e aggiorna la data nel pannello (scheda Rete)."
                           : "Controlla il credito o rinnova il piano.",
             used);
    bool ok = alerts_send_now(s, days_left < 0 ? "EVONETRTK - piano SIM scaduto" : "EVONETRTK - rinnovo piano SIM", body);
    ESP_LOGW(TAG, "Promemoria rinnovo SIM %s: %s",
             ok ? "inviato" : "NON inviato (email/WhatsApp non configurati o in errore)", body);
}

static void sim_plan_task(void *arg)
{
    static app_settings_t s;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(10000));
        uint32_t today = today_ymd();
        s = settings_get();
        rc_set(&s);
        if (!today || !s.sim_renew_date) {
            continue;
        }

        // Rinnovi periodici: il giorno dopo il rinnovo la data passa al
        // ciclo successivo (e i promemoria ripartono per quella).
        int left = days_between(today, s.sim_renew_date);
        if (left < 0 && (s.sim_renew_mode == SIM_RENEW_MONTHLY ||
                         (s.sim_renew_mode == SIM_RENEW_DAYS && s.sim_renew_every_days > 0))) {
            uint32_t next = s.sim_renew_date;
            int guard = 0;
            while (days_between(today, next) < 0 && guard++ < 2000) {
                next = s.sim_renew_mode == SIM_RENEW_MONTHLY
                    ? add_months(next, 1, (int) (s.sim_renew_date % 100))
                    : add_days(next, s.sim_renew_every_days);
            }
            ESP_LOGI(TAG, "Rinnovo della SIM passato: prossimo il %02u/%02u/%04u",
                     (unsigned) (next % 100), (unsigned) (next / 100 % 100), (unsigned) (next / 10000));
            s.sim_renew_date = next;
            settings_save(&s);
            rc_set(&s);
            continue;
        }

        time_t now = time(NULL);
        struct tm tm;
        localtime_r(&now, &tm);
        if (tm.tm_hour < NOTICE_HOUR) {
            continue;
        }

        // Cosa e' gia' stato mandato, per data di rinnovo: in NVS, cosi' un
        // riavvio non fa ripartire lo stesso promemoria.
        nvs_handle_t h;
        if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
            continue;
        }
        uint32_t sent1 = 0, sent2 = 0, sent_exp = 0;
        nvs_get_u32(h, "n1", &sent1);
        nvs_get_u32(h, "n2", &sent2);
        nvs_get_u32(h, "exp", &sent_exp);
        bool changed = false;
        if (left < 0) {
            // Scadenza unica passata: un promemoria al giorno finche' la data
            // non viene aggiornata.
            if (sent_exp != today) {
                send_notice(&s, left);
                nvs_set_u32(h, "exp", today);
                changed = true;
            }
        } else if (left <= s.sim_notice2_days && sent2 != s.sim_renew_date) {
            send_notice(&s, left);
            nvs_set_u32(h, "n2", s.sim_renew_date);
            nvs_set_u32(h, "n1", s.sim_renew_date); // il primo ormai non serve
            changed = true;
        } else if (left <= s.sim_notice1_days && left > s.sim_notice2_days && sent1 != s.sim_renew_date) {
            send_notice(&s, left);
            nvs_set_u32(h, "n1", s.sim_renew_date);
            changed = true;
        }
        if (changed) {
            nvs_commit(h);
        }
        nvs_close(h);
    }
}

void sim_plan_start(void)
{
    static app_settings_t s0;
    s0 = settings_get();
    rc_set(&s0);
    xTaskCreate(sim_plan_task, "sim_plan", 6144, NULL, 2, NULL);
}
