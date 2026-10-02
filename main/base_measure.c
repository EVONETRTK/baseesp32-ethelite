#include "base_measure.h"
#include "settings.h"
#include "sys_stats.h"
#include "gnss_fix.h"
#include "status.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "base_measure";

#define MEASURE_FIXED_TARGET   180          // 3 minuti di fix RTK fisso (GGA a 1 Hz)
#define MEASURE_FIXED_MIN      30           // a tempo scaduto bastano questi fissi
#define MEASURE_FLOAT_MIN      300          // ...altrimenti 5 minuti di float
#define MEASURE_TIMEOUT_S      (20 * 60)
#define M_PER_DEG              111320.0
#define MEASURE_NO_CASTER_S    180          // senza correzioni dal caster si rinuncia prima

// Media e varianza incrementali (Welford) di lat/lon/quota ellissoidica.
typedef struct {
    uint32_t n;
    double mean[3];
    double m2[3];
} acc_t;

static acc_t s_fixed, s_float;
static base_measure_progress_t s_prog = { .quality = -1 };
// Statica: app_settings_t e' ~2 KB, troppi per gli stack dei chiamanti.
static app_settings_t s_cfg;

static void acc_add(acc_t *a, const double v[3])
{
    a->n++;
    for (int i = 0; i < 3; i++) {
        double d = v[i] - a->mean[i];
        a->mean[i] += d / a->n;
        a->m2[i] += d * (v[i] - a->mean[i]);
    }
}

// Deviazione standard orizzontale e verticale in metri.
static void acc_spread_m(const acc_t *a, double *horiz_m, double *vert_m)
{
    if (a->n < 2) {
        *horiz_m = *vert_m = 0;
        return;
    }
    double sd_lat = sqrt(a->m2[0] / (a->n - 1)) * M_PER_DEG;
    double sd_lon = sqrt(a->m2[1] / (a->n - 1)) * M_PER_DEG * cos(a->mean[0] * M_PI / 180.0);
    *horiz_m = sqrt(sd_lat * sd_lat + sd_lon * sd_lon);
    *vert_m = sqrt(a->m2[2] / (a->n - 1));
}

// Torna base (riavviando). Con ok = true salva anche le coordinate come
// posizione fissa.
static void finish(bool ok, const acc_t *a, const char *msg)
{
    s_cfg = settings_get();
    if (ok) {
        s_cfg.base_position_mode = BASE_POSITION_MANUAL;
        s_cfg.base_fixed_lat_deg = a->mean[0];
        s_cfg.base_fixed_lon_deg = a->mean[1];
        s_cfg.base_fixed_height_m = a->mean[2];
    }
    s_cfg.device_mode = DEVICE_MODE_BASE;
    s_cfg.base_measure_active = false;
    strncpy(s_cfg.base_measure_msg, msg, sizeof(s_cfg.base_measure_msg) - 1);
    s_cfg.base_measure_msg[sizeof(s_cfg.base_measure_msg) - 1] = '\0';
    settings_save(&s_cfg);
    if (ok) {
        ESP_LOGI(TAG, "Posizione base salvata: lat %.9f lon %.9f quota ellissoidica %.4f m", a->mean[0], a->mean[1], a->mean[2]);
    }
    ESP_LOGW(TAG, "%s - riavvio come base", msg);
    sys_stats_note_restart_reason(ok ? "misura della posizione base completata" : "misura della posizione base terminata senza esito");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void measure_task(void *arg)
{
    int64_t start_us = esp_timer_get_time();
    int64_t last_seen_us = 0;
    bool never_connected = true;
    char msg[128];

    ESP_LOGI(TAG, "Misura della posizione base in corso: servono %d s di fix RTK fisso (massimo %d min)",
             MEASURE_FIXED_TARGET, MEASURE_TIMEOUT_S / 60);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        uint32_t elapsed = (uint32_t) ((esp_timer_get_time() - start_us) / 1000000);
        s_prog.elapsed_s = elapsed;

        gnss_fix_status_t fx = gnss_fix_get_status();
        if (fx.valid && fx.last_update_us != last_seen_us) {
            last_seen_us = fx.last_update_us;
            s_prog.quality = (int) fx.quality;
            if (fx.has_position && fx.altitude_m > -9999 && fx.geoid_sep_m > -9999) {
                // Quota ellissoidica: quella che il ricevitore ha calcolato,
                // ricostruita da GGA (quota sul geoide + separazione).
                double v[3] = { fx.lat_deg, fx.lon_deg, (double) fx.altitude_m + (double) fx.geoid_sep_m };
                if (fx.quality == GNSS_FIX_RTK_FIXED) {
                    acc_add(&s_fixed, v);
                } else if (fx.quality == GNSS_FIX_RTK_FLOAT) {
                    acc_add(&s_float, v);
                }
                s_prog.fixed_n = s_fixed.n;
                s_prog.float_n = s_float.n;
            }
        }

        // Senza correzioni (es. password del rover rifiutata) il fix RTK non
        // arrivera' mai: inutile aspettare 20 minuti. Visto alla prima
        // prova: caster che rifiutava le credenziali per tutta la misura.
        ntrip_conn_status_t nt = status_ntrip_get();
        if (nt.connected) {
            never_connected = false;
        } else if (never_connected && elapsed >= MEASURE_NO_CASTER_S) {
            snprintf(msg, sizeof(msg), "Misura fallita: nessuna correzione dal caster in %d min (%.70s)",
                     MEASURE_NO_CASTER_S / 60, nt.last_error[0] ? nt.last_error : "nessuna risposta");
            finish(false, NULL, msg);
        }

        double h, vt;
        if (s_fixed.n >= MEASURE_FIXED_TARGET) {
            acc_spread_m(&s_fixed, &h, &vt);
            snprintf(msg, sizeof(msg), "Misura riuscita con RTK fisso: %u campioni, dispersione %.1f cm orizz. / %.1f cm vert.",
                     (unsigned) s_fixed.n, h * 100, vt * 100);
            finish(true, &s_fixed, msg);
        }
        if (elapsed >= MEASURE_TIMEOUT_S) {
            if (s_fixed.n >= MEASURE_FIXED_MIN) {
                acc_spread_m(&s_fixed, &h, &vt);
                snprintf(msg, sizeof(msg), "Misura riuscita con RTK fisso (solo %u campioni in %d min): dispersione %.1f cm orizz. / %.1f cm vert.",
                         (unsigned) s_fixed.n, MEASURE_TIMEOUT_S / 60, h * 100, vt * 100);
                finish(true, &s_fixed, msg);
            } else if (s_float.n >= MEASURE_FLOAT_MIN) {
                acc_spread_m(&s_float, &h, &vt);
                snprintf(msg, sizeof(msg), "Misura con solo RTK float (%u campioni): precisione di decimetri, dispersione %.0f cm orizz. / %.0f cm vert.",
                         (unsigned) s_float.n, h * 100, vt * 100);
                finish(true, &s_float, msg);
            } else {
                snprintf(msg, sizeof(msg), "Misura fallita: nessun fix RTK in %d min (fisso %u, float %u campioni). Controlla correzioni e cielo.",
                         MEASURE_TIMEOUT_S / 60, (unsigned) s_fixed.n, (unsigned) s_float.n);
                finish(false, NULL, msg);
            }
        }
    }
}

bool base_measure_request_start(char *err, size_t err_size)
{
    s_cfg = settings_get();
    if (s_cfg.gnss_chip != GNSS_CHIP_UBLOX && s_cfg.gnss_chip != GNSS_CHIP_LC29H) {
        snprintf(err, err_size, "La posizione fissa e' supportata solo con ricevitori u-blox e Quectel LC29H");
        return false;
    }
    if (!s_cfg.rover_mountpoint[0]) {
        snprintf(err, err_size, "Manca il mountpoint del rover (GNSS & NTRIP): serve una stazione da cui ricevere le correzioni");
        return false;
    }
    s_cfg.base_measure_active = true;
    s_cfg.device_mode = DEVICE_MODE_ROVER;
    snprintf(s_cfg.base_measure_msg, sizeof(s_cfg.base_measure_msg), "Misura in corso (correzioni da /%s)", s_cfg.rover_mountpoint);
    if (settings_save(&s_cfg) != ESP_OK) {
        snprintf(err, err_size, "Salvataggio impostazioni fallito");
        return false;
    }
    ESP_LOGI(TAG, "Misura della posizione base richiesta: riavvio come rover su /%s", s_cfg.rover_mountpoint);
    return true;
}

void base_measure_request_cancel(void)
{
    s_cfg = settings_get();
    if (!s_cfg.base_measure_active) {
        return;
    }
    s_cfg.base_measure_active = false;
    s_cfg.device_mode = DEVICE_MODE_BASE;
    snprintf(s_cfg.base_measure_msg, sizeof(s_cfg.base_measure_msg), "Misura annullata, coordinate non modificate");
    settings_save(&s_cfg);
    ESP_LOGI(TAG, "Misura della posizione base annullata");
}

void base_measure_start_if_active(void)
{
    s_cfg = settings_get();
    if (!s_cfg.base_measure_active) {
        return;
    }
    s_prog.active = true;
    s_prog.target_n = MEASURE_FIXED_TARGET;
    s_prog.timeout_s = MEASURE_TIMEOUT_S;
    xTaskCreate(measure_task, "base_measure", 4096, NULL, 3, NULL);
}

base_measure_progress_t base_measure_get_progress(void)
{
    return s_prog;
}
