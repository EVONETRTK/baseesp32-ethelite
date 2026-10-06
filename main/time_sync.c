#include "time_sync.h"

#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <sys/time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_sntp.h"

static const char *TAG = "time_sync";

static volatile bool s_valid;
static volatile bool s_ntp;          // l'NTP ha risposto almeno una volta
static volatile int64_t s_gnss_set_us; // ultima regolazione dai satelliti (esp_timer)

static void on_sync(struct timeval *tv)
{
    bool first = !s_valid || !s_ntp;
    s_valid = true;
    s_ntp = true;
    if (first) {
        char now[24];
        time_sync_format_now(now, sizeof(now));
        // Riga di riferimento: nei log precedenti l'ora e' quella
        // dall'accensione, da qui in poi e' l'ora vera.
        ESP_LOGI(TAG, "Ora sincronizzata via NTP: %s (accesa da %lld s)", now,
                 (long long) (esp_timer_get_time() / 1000000));
    }
}

void time_sync_set_timezone(void)
{
    // Ora legale italiana (ultima domenica di marzo / di ottobre).
    setenv("TZ", "CET-1CEST,M3.5.0,M10.5.0/3", 1);
    tzset();
}

void time_sync_start(void)
{
    time_sync_set_timezone();
    esp_sntp_setoperatingmode(ESP_SNTP_OPMODE_POLL);
    esp_sntp_setservername(0, "pool.ntp.org");
    esp_sntp_setservername(1, "time.google.com");
    sntp_set_time_sync_notification_cb(on_sync);
    esp_sntp_init(); // ritenta da solo finche' la rete non c'e'
}

bool time_sync_is_valid(void)
{
    return s_valid;
}

const char *time_sync_source(void)
{
    return !s_valid ? "" : (s_ntp ? "NTP" : "GNSS");
}

bool time_sync_wants_gnss(void)
{
    return !s_ntp;
}

// Giorni dal 1970-01-01 per una data del calendario gregoriano (UTC), senza
// timegm() (non sempre disponibile) e senza toccare il fuso orario.
static int64_t days_from_civil(int y, int m, int d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const int64_t yoe = y - era * 400;
    const int64_t doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + doe - 719468;
}

void time_sync_from_gnss(int year, int month, int day, int hour, int min, int sec)
{
    if (s_ntp || year < 2024 || year > 2099 || month < 1 || month > 12 || day < 1 || day > 31 ||
        hour > 23 || min > 59 || sec > 60) {
        return;
    }
    int64_t now_us = esp_timer_get_time();
    // Una volta regolata, si riallinea al massimo ogni 10 minuti.
    if (s_valid && s_gnss_set_us && now_us - s_gnss_set_us < 10LL * 60 * 1000000) {
        return;
    }
    int64_t epoch = days_from_civil(year, month, day) * 86400 + hour * 3600 + min * 60 + sec;
    struct timeval tv = { .tv_sec = (time_t) epoch, .tv_usec = 0 };
    settimeofday(&tv, NULL);
    s_gnss_set_us = now_us;
    if (!s_valid) {
        s_valid = true;
        char now[24];
        time_sync_format_now(now, sizeof(now));
        ESP_LOGI(TAG, "Ora presa dai satelliti (GNSS), in attesa dell'NTP: %s (accesa da %lld s)", now,
                 (long long) (now_us / 1000000));
    }
}

static void format_epoch(time_t t, char *out, size_t out_size)
{
    struct tm tm;
    localtime_r(&t, &tm);
    strftime(out, out_size, "%Y-%m-%d %H:%M:%S", &tm);
}

void time_sync_format_now(char *out, size_t out_size)
{
    if (!s_valid) {
        if (out_size) out[0] = '\0';
        return;
    }
    format_epoch(time(NULL), out, out_size);
}

void time_sync_format_uptime(int64_t uptime_us, char *out, size_t out_size)
{
    if (!s_valid) {
        if (out_size) out[0] = '\0';
        return;
    }
    int64_t ago_s = (esp_timer_get_time() - uptime_us) / 1000000;
    format_epoch(time(NULL) - (time_t) ago_s, out, out_size);
}
