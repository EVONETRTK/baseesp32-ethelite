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

static void on_sync(struct timeval *tv)
{
    bool first = !s_valid;
    s_valid = true;
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
