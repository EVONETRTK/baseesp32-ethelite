#include "base_measure.h"
#include "license.h"
#include "gnss_detect.h"
#include "settings.h"
#include "sys_stats.h"
#include "gnss_fix.h"
#include "status.h"
#include "etrf.h"
#include "gnss_unicore.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "base_measure";

#define MEASURE_FIXED_TARGET   180          // 3 minuti di fix RTK fisso (GGA a 1 Hz)
#define MEASURE_FIXED_MIN      30           // a tempo scaduto bastano questi fissi
#define MEASURE_FLOAT_MIN      300          // ...altrimenti 5 minuti di float
#define MEASURE_TIMEOUT_S      (20 * 60)
#define M_PER_DEG              111320.0
#define MEASURE_NO_CASTER_S    180          // senza correzioni dal caster si rinuncia prima

// Misura con Galileo HAS (PPP): il ricevitore converge in decine di minuti,
// poi si fa la media per ore. Dopo ogni avvio la prima mezz'ora si scarta.
// La soluzione HAS si legge dal log PPPNAVA dell'UM98x (gnss_unicore.h): la
// GGA resta la soluzione autonoma (prova del 07/10/2026 sulla base ALTAMURA).
#define HAS_WARMUP_S           (30 * 60)
#define HAS_DEFAULT_HOURS      6
#define HAS_MIN_PPP_N          1800         // almeno 30 minuti di soluzioni HAS buone (PPPNAVA a 1 Hz)
#define HAS_MAX_SIGMA_H_M      1.5f         // incertezza orizzontale dichiarata massima per usare l'epoca
#define HAS_MAX_CORR_AGE_S     30.0f        // correzioni piu' vecchie: salti di 15 m visti il 07/10
#define HAS_JUMP_AFTER_N       300          // dopo 5 min di epoche buone si scartano i salti dalla media
#define HAS_JUMP_K             3.0          // ...oltre 3 volte l'incertezza dichiarata
#define HAS_JUMP_MIN_H_M       2.0          // ...e comunque oltre 2 m in orizzontale / 3 m in quota:
#define HAS_JUMP_MIN_V_M       3.0          // il 07/10 un salto di 15 m aveva incertezza dichiarata 1,4 m
#define HAS_NO_DATA_S          600          // nessuna soluzione PPP per 10 min: si rinuncia
#define HAS_SAVE_EVERY_S       600          // avanzamento salvato ogni 10 min: riprende dopo un riavvio
#define HAS_NVS_NS             "basemeas"
#define HAS_MAGIC              0x48415332u  // "HAS2" (la HAS1 leggeva la GGA)

// Media e varianza incrementali (Welford) di lat/lon/quota ellissoidica.
typedef struct {
    uint32_t n;
    double mean[3];
    double m2[3];
} acc_t;

static acc_t s_fixed, s_float;
static base_measure_progress_t s_prog = { .quality = -1 };
// Nessuna copia di app_settings_t (~2,1 KB): letture e modifiche sul posto
// con settings_peek()/settings_update().

// Avanzamento della misura HAS, salvato in NVS (una misura di ore non deve
// ripartire da zero per una mancanza di corrente).
typedef struct {
    uint32_t magic;
    uint32_t done_s;   // secondi di misura dopo la convergenza, sommati sugli avvii
    acc_t ppp;         // epoche HAS buone (dispersione, non pesate)
    double sw;         // media pesata con 1/sigma^2 (posizione finale)
    double swx[3];
    double sum_sig_h;  // per l'incertezza media dichiarata
    uint32_t seen;     // tutte le righe PPPNAVA dopo la convergenza
    uint32_t rej_sigma, rej_age, rej_type; // scartate: incertezza, correzioni vecchie, non PPP
    uint32_t rej_jump; // scartate: salti lontani dalla media
} has_saved_t;

static has_saved_t s_has;

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
typedef struct {
    bool ok;
    double pos[3];
    const char *msg;
} finish_ctx_t;

static void apply_finish(app_settings_t *s, void *ctx)
{
    finish_ctx_t *c = ctx;
    if (c->ok) {
        s->base_position_mode = BASE_POSITION_MANUAL;
        s->base_fixed_lat_deg = c->pos[0];
        s->base_fixed_lon_deg = c->pos[1];
        s->base_fixed_height_m = c->pos[2];
    }
    s->device_mode = DEVICE_MODE_BASE;
    s->base_measure_active = false;
    strncpy(s->base_measure_msg, c->msg, sizeof(s->base_measure_msg) - 1);
    s->base_measure_msg[sizeof(s->base_measure_msg) - 1] = '\0';
}

static void finish_at(bool ok, const double pos[3], const char *msg)
{
    finish_ctx_t c = { .ok = ok, .msg = msg };
    if (ok) {
        memcpy(c.pos, pos, sizeof(c.pos));
    }
    settings_update(apply_finish, &c);
    if (ok) {
        ESP_LOGI(TAG, "Posizione base salvata: lat %.9f lon %.9f quota ellissoidica %.4f m", pos[0], pos[1], pos[2]);
    }
    ESP_LOGW(TAG, "%s - riavvio come base", msg);
    sys_stats_note_restart_reason(ok ? "misura della posizione base completata" : "misura della posizione base terminata senza esito");
    vTaskDelay(pdMS_TO_TICKS(1000));
    esp_restart();
}

static void finish(bool ok, const acc_t *a, const char *msg)
{
    finish_at(ok, a ? a->mean : NULL, msg);
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

// --- misura con Galileo HAS ------------------------------------------------

static void has_load(void)
{
    nvs_handle_t h;
    size_t len = sizeof(s_has);
    memset(&s_has, 0, sizeof(s_has));
    if (nvs_open(HAS_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_blob(h, "has", &s_has, &len) != ESP_OK || len != sizeof(s_has) || s_has.magic != HAS_MAGIC) {
            memset(&s_has, 0, sizeof(s_has));
        }
        nvs_close(h);
    }
    s_has.magic = HAS_MAGIC;
}

static void has_save(void)
{
    nvs_handle_t h;
    if (nvs_open(HAS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_blob(h, "has", &s_has, sizeof(s_has));
        nvs_commit(h);
        nvs_close(h);
    }
}

static void has_clear(void)
{
    nvs_handle_t h;
    if (nvs_open(HAS_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_key(h, "has");
        nvs_commit(h);
        nvs_close(h);
    }
}

// Salto: posizione lontana dalla media pesata raccolta finora. La sola
// incertezza dichiarata non basta: il 07/10 il ricevitore ha dato una
// posizione spostata di 15 m dichiarando 1,4 m.
static bool has_is_jump(const gnss_unicore_ppp_t *p, float sig_h)
{
    if (s_has.ppp.n < HAS_JUMP_AFTER_N || s_has.sw <= 0) {
        return false;
    }
    double lat = s_has.swx[0] / s_has.sw, lon = s_has.swx[1] / s_has.sw, h = s_has.swx[2] / s_has.sw;
    double dn = (p->lat_deg - lat) * M_PER_DEG;
    double de = (p->lon_deg - lon) * M_PER_DEG * cos(lat * M_PI / 180.0);
    double lim_h = fmax(HAS_JUMP_K * sig_h, HAS_JUMP_MIN_H_M);
    double lim_v = fmax(HAS_JUMP_K * p->sig_h_m, HAS_JUMP_MIN_V_M);
    return hypot(dn, de) > lim_h || fabs(p->h_ell_m - h) > lim_v;
}

static void has_finish(void)
{
    char msg[128];
    uint32_t good = s_has.ppp.n;
    ESP_LOGI(TAG, "Fine misura HAS: %u righe PPPNAVA, %u buone; scartate %u per incertezza, %u per correzioni vecchie, %u non PPP, %u salti",
             (unsigned) s_has.seen, (unsigned) good, (unsigned) s_has.rej_sigma, (unsigned) s_has.rej_age,
             (unsigned) s_has.rej_type, (unsigned) s_has.rej_jump);

    if (good < HAS_MIN_PPP_N || s_has.sw <= 0) {
        snprintf(msg, sizeof(msg), "Misura HAS fallita: solo %u min di soluzioni HAS buone (servono %d). Antenna, cielo o E6? Coordinate invariate.",
                 (unsigned) (good / 60), HAS_MIN_PPP_N / 60);
        has_clear();
        finish(false, NULL, msg);
    }
    time_t now = time(NULL);
    if (now < 1700000000) {
        snprintf(msg, sizeof(msg), "Misura HAS fallita: ora non valida, impossibile convertire in ETRF2000. Coordinate invariate.");
        has_clear();
        finish(false, NULL, msg);
    }

    double h, vt;
    acc_spread_m(&s_has.ppp, &h, &vt);
    double itrf[3], pos[3];
    for (int i = 0; i < 3; i++) {
        itrf[i] = pos[i] = s_has.swx[i] / s_has.sw; // media pesata con 1/sigma^2
    }
    double epoch = etrf_decimal_year(now);
    etrf_itrf2020_to_etrf2000(&pos[0], &pos[1], &pos[2], epoch);
    double dn = (pos[0] - itrf[0]) * M_PER_DEG;
    double de = (pos[1] - itrf[1]) * M_PER_DEG * cos(pos[0] * M_PI / 180.0);
    double sig = s_has.sum_sig_h / good;
    ESP_LOGI(TAG, "HAS media pesata ITRF2020 (epoca %.2f): lat %.9f lon %.9f h %.4f -> ETRF2000: lat %.9f lon %.9f h %.4f (spostamento %.2f m)",
             epoch, itrf[0], itrf[1], itrf[2], pos[0], pos[1], pos[2], hypot(dn, de));
    snprintf(msg, sizeof(msg), "Misura HAS riuscita: %u h, incertezza media %.0f cm, dispersione %.0f/%.0f cm, convertita in ETRF2000 (%.0f cm).",
             (unsigned) (s_has.done_s / 3600), sig * 100, h * 100, vt * 100, hypot(dn, de) * 100);
    has_clear();
    finish_at(true, pos, msg);
}

static void has_task(void *arg)
{
    has_load();
    const uint32_t done_at_boot = s_has.done_s;
    uint32_t last_save = done_at_boot;
    int64_t start_us = esp_timer_get_time();
    int64_t last_seen_us = 0, last_ppp_us = start_us, last_gga_us = 0;
    char msg[128];

    ESP_LOGI(TAG, "Misura con Galileo HAS: %u s gia' fatti, obiettivo %u s; la prima mezz'ora dopo l'avvio si scarta",
             (unsigned) done_at_boot, (unsigned) s_prog.has_target_s);

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(200));
        int64_t now_us = esp_timer_get_time();
        uint32_t elapsed = (uint32_t) ((now_us - start_us) / 1000000);
        bool converging = elapsed < HAS_WARMUP_S;
        s_prog.elapsed_s = elapsed;
        s_prog.has_warmup_left_s = converging ? HAS_WARMUP_S - elapsed : 0;
        s_has.done_s = done_at_boot + (converging ? 0 : elapsed - HAS_WARMUP_S);
        s_prog.has_done_s = s_has.done_s;

        gnss_fix_status_t fx = gnss_fix_get_status();
        if (fx.valid && fx.last_update_us != last_gga_us) {
            last_gga_us = fx.last_update_us;
            s_prog.quality = (int) fx.quality; // solo diagnostica: la posizione HAS non e' nella GGA
        }

        // Soluzione HAS dal log PPPNAVA (una riga al secondo).
        gnss_unicore_ppp_t p;
        gnss_unicore_ppp_get(&p);
        if (p.at_us && p.at_us != last_seen_us) {
            last_seen_us = p.at_us;
            last_ppp_us = now_us;
            float sig_h = hypotf(p.sig_lat_m, p.sig_lon_m);
            s_prog.has_sigma_cm = p.valid ? (uint32_t) (sig_h * 100) : 0;
            s_prog.has_corr_age_s = (uint32_t) p.corr_age_s;
            if (!converging) {
                s_has.seen++;
                if (!p.valid) {
                    s_has.rej_type++;
                } else if (p.corr_age_s > HAS_MAX_CORR_AGE_S) {
                    s_has.rej_age++;
                } else if (sig_h > HAS_MAX_SIGMA_H_M || sig_h <= 0) {
                    s_has.rej_sigma++;
                } else if (has_is_jump(&p, sig_h)) {
                    s_has.rej_jump++;
                } else {
                    double v[3] = { p.lat_deg, p.lon_deg, p.h_ell_m };
                    double w = 1.0 / ((double) sig_h * sig_h);
                    acc_add(&s_has.ppp, v);
                    s_has.sw += w;
                    for (int i = 0; i < 3; i++) {
                        s_has.swx[i] += w * v[i];
                    }
                    s_has.sum_sig_h += sig_h;
                }
            }
            s_prog.fixed_n = s_has.ppp.n;
            s_prog.float_n = s_has.seen;
        }

        if (now_us - last_ppp_us > (int64_t) HAS_NO_DATA_S * 1000000) {
            snprintf(msg, sizeof(msg), "Misura HAS fallita: nessuna soluzione PPP dal ricevitore da %d min (collegamento? ricevitore Unicore?). Coordinate invariate.",
                     HAS_NO_DATA_S / 60);
            has_clear();
            finish(false, NULL, msg);
        }
        if (s_has.done_s - last_save >= HAS_SAVE_EVERY_S) {
            last_save = s_has.done_s;
            has_save();
        }
        if (s_has.done_s >= s_prog.has_target_s) {
            has_finish();
        }
    }
}

// --- avvio e annullamento ---------------------------------------------------

typedef struct {
    gnss_chip_t chip;
    bool active;
    uint8_t method;
    uint8_t has_hours;
    char mountpoint[33];
} measure_view_t;

static void peek_view(const app_settings_t *s, void *ctx)
{
    measure_view_t *v = ctx;
    v->chip = gnss_detect_effective(s->gnss_chip);
    v->active = s->base_measure_active;
    v->method = s->base_measure_method;
    v->has_hours = s->base_measure_has_hours;
    memcpy(v->mountpoint, s->rover_mountpoint, sizeof(v->mountpoint));
    v->mountpoint[sizeof(v->mountpoint) - 1] = '\0';
}

static void apply_start(app_settings_t *s, void *ctx)
{
    s->base_measure_active = true;
    s->base_measure_method = BASE_MEASURE_RTK;
    s->device_mode = DEVICE_MODE_ROVER;
    snprintf(s->base_measure_msg, sizeof(s->base_measure_msg), "Misura in corso (correzioni da /%s)", s->rover_mountpoint);
}

static void apply_start_has(app_settings_t *s, void *ctx)
{
    int hours = *(const int *) ctx;
    s->base_measure_active = true;
    s->base_measure_method = BASE_MEASURE_HAS;
    s->base_measure_has_hours = (uint8_t) hours;
    s->device_mode = DEVICE_MODE_ROVER;
    snprintf(s->base_measure_msg, sizeof(s->base_measure_msg), "Misura con Galileo HAS in corso (%d ore)", hours);
}

static void apply_cancel(app_settings_t *s, void *ctx)
{
    s->base_measure_active = false;
    s->device_mode = DEVICE_MODE_BASE;
    snprintf(s->base_measure_msg, sizeof(s->base_measure_msg), "Misura annullata, coordinate non modificate");
}

bool base_measure_request_start(char *err, size_t err_size)
{
    if (!license_has(LIC_BASE)) {
        snprintf(err, err_size, "La misura della posizione fa parte del Pacchetto Base, non compreso nella licenza");
        return false;
    }
    measure_view_t v;
    settings_peek(peek_view, &v);
    if (v.chip != GNSS_CHIP_UBLOX && v.chip != GNSS_CHIP_LC29H && v.chip != GNSS_CHIP_UNICORE && v.chip != GNSS_CHIP_COMNAV) {
        snprintf(err, err_size, "La posizione fissa e' supportata solo con ricevitori u-blox, Unicore, ComNav e Quectel LC29H");
        return false;
    }
    if (!v.mountpoint[0]) {
        snprintf(err, err_size, "Manca il mountpoint del rover (scheda Posizione e correzioni, Client NTRIP): serve una stazione da cui ricevere le correzioni");
        return false;
    }
    if (settings_update(apply_start, NULL) != ESP_OK) {
        snprintf(err, err_size, "Salvataggio impostazioni fallito");
        return false;
    }
    ESP_LOGI(TAG, "Misura della posizione base richiesta: riavvio come rover su /%s", v.mountpoint);
    return true;
}

bool base_measure_request_start_has(int hours, char *err, size_t err_size)
{
    if (!license_has(LIC_BASE)) {
        snprintf(err, err_size, "La misura con Galileo HAS fa parte del Pacchetto Base, non compreso nella licenza");
        return false;
    }
    measure_view_t v;
    settings_peek(peek_view, &v);
    if (v.chip != GNSS_CHIP_UNICORE && v.chip != GNSS_CHIP_COMNAV) {
        snprintf(err, err_size, "La misura con Galileo HAS richiede un ricevitore Unicore UM980/UM982 o ComNav K922: lo ZED-F9P non riceve il segnale E6");
        return false;
    }
    if (hours < 1) hours = 1;
    if (hours > 24) hours = 24;
    has_clear();
    if (settings_update(apply_start_has, &hours) != ESP_OK) {
        snprintf(err, err_size, "Salvataggio impostazioni fallito");
        return false;
    }
    ESP_LOGI(TAG, "Misura della posizione base con Galileo HAS richiesta (%d ore): riavvio", hours);
    return true;
}

void base_measure_request_cancel(void)
{
    measure_view_t v;
    settings_peek(peek_view, &v);
    if (!v.active) {
        return;
    }
    settings_update(apply_cancel, NULL);
    has_clear();
    ESP_LOGI(TAG, "Misura della posizione base annullata");
}

bool base_measure_is_active(void)
{
    measure_view_t v;
    settings_peek(peek_view, &v);
    return v.active;
}

bool base_measure_is_has(void)
{
    measure_view_t v;
    settings_peek(peek_view, &v);
    return v.active && v.method == BASE_MEASURE_HAS;
}

void base_measure_start_if_active(void)
{
    measure_view_t v;
    settings_peek(peek_view, &v);
    if (!v.active) {
        return;
    }
    // Misura avviata prima (licenza poi scaduta, revocata o dimenticata): al
    // riavvio si ricontrolla il Pacchetto Base. Senza, si annulla e si torna
    // base con le coordinate di prima (finish_at riavvia).
    if (!license_has(LIC_BASE)) {
        has_clear();
        finish_at(false, NULL, "Misura annullata: il Pacchetto Base non e' compreso nella licenza, coordinate non modificate");
        return;
    }
    s_prog.active = true;
    s_prog.method = v.method;
    if (v.method == BASE_MEASURE_HAS) {
        s_prog.has_target_s = (uint32_t) (v.has_hours ? v.has_hours : HAS_DEFAULT_HOURS) * 3600;
        s_prog.has_warmup_left_s = HAS_WARMUP_S;
        xTaskCreate(has_task, "base_has", 4096, NULL, 3, NULL);
        return;
    }
    s_prog.target_n = MEASURE_FIXED_TARGET;
    s_prog.timeout_s = MEASURE_TIMEOUT_S;
    xTaskCreate(measure_task, "base_measure", 4096, NULL, 3, NULL);
}

base_measure_progress_t base_measure_get_progress(void)
{
    return s_prog;
}
