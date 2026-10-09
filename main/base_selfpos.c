#include "base_selfpos.h"
#include "base_monitor.h"
#include "etrf.h"
#include "settings.h"

#include <math.h>
#include <string.h>
#include <stdlib.h>
#include <time.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "freertos/message_buffer.h"

static const char *TAG = "base_selfpos";

// Prototipo e prove sul PC: scratchpad "drift/spp2.js" della sessione dell'08/10/2026
// (stessi algoritmi, stessi numeri sulla registrazione del 04/10).

#define MAX_PAYLOAD     4096        // RAWX = 16 + 32 * misure: 4 costellazioni ~ 2,5 KB
#define QUEUE_BYTES     (24 * 1024) // nella PSRAM
#define RAWX_RATE       5           // una soluzione ogni 5 epoche (5 s con la base a 1 Hz)
#define WINDOW_S        (30 * 60)
#define MIN_SAMPLES     120         // almeno 10 minuti di soluzioni buone nella finestra
#define MAX_SAMPLES     512
#define CNO_MIN         25
#define ELMASK_DEG      10.0
#define MAX_OBS         40
#define ALARM_WINDOWS   2

#define GPS_N 32
#define GAL_N 36

#define CLIGHT   299792458.0
#define SC_PI    3.1415926535898
#define MU_GPS   3.986005e14
#define MU_GAL   3.986004418e14
#define OMGE     7.2921151467e-5
#define F_REL    (-4.442807633e-10)
#define WGS_A    6378137.0
#define WGS_F    (1.0 / 298.257223563)
#define WGS_E2   (WGS_F * (2.0 - WGS_F))

typedef struct {
    bool valid;
    double mu;
    double toe, toc, f0, f1, f2, tgd;
    double M0, deln, e, sqrtA, OMG0, i0, omg, OMGd, idot;
    double cuc, cus, crc, crs, cic, cis;
    double t_dec;   // tempo GPS assoluto (s) della decodifica, 0 = prima del primo RAWX
} eph_t;

// Eta' massima di un'effemeride (dalla decodifica): sat_pos() controlla solo
// |t - toe| dentro la settimana, quindi un'effemeride rimasta in RAM da
// esattamente 7 giorni prima passava il controllo con il satellite a
// migliaia di km dalla posizione vera.
#define EPH_MAX_AGE_S (6 * 3600.0)

typedef struct {
    const eph_t *e;
    double pr;
    bool gal;
    bool reject;
} obs_t;

// --- stato condiviso ---------------------------------------------------------
static SemaphoreHandle_t s_mutex;
static base_selfpos_status_t s_st;
static volatile bool s_enabled;
static bool s_fixed_etrf;

// --- assemblaggio dei frame (solo nel task che legge il ricevitore) ----------
static uint8_t *s_frame;
static size_t s_pos, s_need;
static MessageBufferHandle_t s_queue;

// --- stato del task di calcolo -------------------------------------------------
// Le tabelle (~20 KB) stanno nella PSRAM: la RAM interna serve al WiFi e
// agli stack (con le tabelle statiche la minima era scesa a 10 KB).
static uint8_t *s_work;
static eph_t *s_gps, *s_gal;              // [GPS_N + 1], [GAL_N + 1]
static uint8_t (*s_gps_sub)[3][30];       // [GPS_N + 1]
static uint8_t s_gps_sub_ok[GPS_N + 1];
static uint8_t (*s_gal_word)[5][16];      // [GAL_N + 1]
static uint8_t s_gal_word_ok[GAL_N + 1];
static double s_ion[8];
static bool s_ion_ok;
static obs_t *s_obs;                      // [MAX_OBS]
static double s_x[5];             // x, y, z, orologio GPS, orologio Galileo (m)
static bool s_x_ok;
static float *s_se, *s_sn, *s_su; // campioni della finestra (PSRAM)
static float *s_sort;
static app_settings_t *s_cfg;     // PSRAM: troppo grande per lo stack del task
static double s_now_abs;          // tempo GPS assoluto dell'ultimo RAWX (settimana * 604800 + tow)

// =============================================================================
// Bit
static uint32_t getbitu(const uint8_t *b, int pos, int len)
{
    uint32_t v = 0;
    for (int i = pos; i < pos + len; i++) {
        v = (v << 1) | ((b[i >> 3] >> (7 - (i & 7))) & 1u);
    }
    return v;
}

static int32_t getbits(const uint8_t *b, int pos, int len)
{
    uint32_t u = getbitu(b, pos, len);
    if (len < 32 && (u & (1u << (len - 1)))) {
        return (int32_t) (u | (~0u << len));
    }
    return (int32_t) u;
}

static void setbit(uint8_t *b, int pos, int v)
{
    if (v) {
        b[pos >> 3] |= (uint8_t) (1u << (7 - (pos & 7)));
    } else {
        b[pos >> 3] &= (uint8_t) ~(1u << (7 - (pos & 7)));
    }
}

static uint32_t crc24q(const uint8_t *b, int n)
{
    uint32_t crc = 0;
    for (int i = 0; i < n; i++) {
        crc ^= (uint32_t) b[i] << 16;
        for (int j = 0; j < 8; j++) {
            crc <<= 1;
            if (crc & 0x1000000u) {
                crc ^= 0x1864cfbu;
            }
        }
    }
    return crc & 0xffffffu;
}

// =============================================================================
// GPS LNAV (sottoframe 1-3: effemeride; 4 pagina 56: Klobuchar)
static void decode_lnav(int sv, const uint32_t *words)
{
    if (sv < 1 || sv > GPS_N) {
        return;
    }
    uint8_t b[30];
    for (int i = 0; i < 10; i++) {
        uint32_t w = (words[i] >> 6) & 0xffffffu;
        b[i * 3] = (uint8_t) (w >> 16);
        b[i * 3 + 1] = (uint8_t) (w >> 8);
        b[i * 3 + 2] = (uint8_t) w;
    }
    if (getbitu(b, 0, 8) != 0x8b) {
        return;
    }
    int id = (int) getbitu(b, 43, 3);
    if (id == 4) {
        if (getbitu(b, 50, 6) == 56) {
            int i = 56;
            s_ion[0] = getbits(b, i, 8) * 0x1p-30;
            s_ion[1] = getbits(b, i + 8, 8) * 0x1p-27;
            s_ion[2] = getbits(b, i + 16, 8) * 0x1p-24;
            s_ion[3] = getbits(b, i + 24, 8) * 0x1p-24;
            s_ion[4] = getbits(b, i + 32, 8) * 0x1p11;
            s_ion[5] = getbits(b, i + 40, 8) * 0x1p14;
            s_ion[6] = getbits(b, i + 48, 8) * 0x1p16;
            s_ion[7] = getbits(b, i + 56, 8) * 0x1p16;
            s_ion_ok = true;
        }
        return;
    }
    if (id < 1 || id > 3) {
        return;
    }
    memcpy(s_gps_sub[sv][id - 1], b, 30);
    s_gps_sub_ok[sv] |= (uint8_t) (1u << (id - 1));
    if (s_gps_sub_ok[sv] != 7) {
        return;
    }
    const uint8_t *b1 = s_gps_sub[sv][0], *b2 = s_gps_sub[sv][1], *b3 = s_gps_sub[sv][2];
    eph_t e = { .mu = MU_GPS };
    int i = 48 + 10 + 2 + 4;
    int svh = (int) getbitu(b1, i, 6); i += 6;
    int iodc0 = (int) getbitu(b1, i, 2); i += 2 + 1 + 87;
    e.tgd = getbits(b1, i, 8) * 0x1p-31; i += 8;
    int iodc1 = (int) getbitu(b1, i, 8); i += 8;
    e.toc = getbitu(b1, i, 16) * 16.0; i += 16;
    e.f2 = getbits(b1, i, 8) * 0x1p-55; i += 8;
    e.f1 = getbits(b1, i, 16) * 0x1p-43; i += 16;
    e.f0 = getbits(b1, i, 22) * 0x1p-31;
    i = 48;
    int iode2 = (int) getbitu(b2, i, 8); i += 8;
    e.crs = getbits(b2, i, 16) * 0x1p-5; i += 16;
    e.deln = getbits(b2, i, 16) * 0x1p-43 * SC_PI; i += 16;
    e.M0 = getbits(b2, i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.cuc = getbits(b2, i, 16) * 0x1p-29; i += 16;
    e.e = getbitu(b2, i, 32) * 0x1p-33; i += 32;
    e.cus = getbits(b2, i, 16) * 0x1p-29; i += 16;
    e.sqrtA = getbitu(b2, i, 32) * 0x1p-19; i += 32;
    e.toe = getbitu(b2, i, 16) * 16.0;
    i = 48;
    e.cic = getbits(b3, i, 16) * 0x1p-29; i += 16;
    e.OMG0 = getbits(b3, i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.cis = getbits(b3, i, 16) * 0x1p-29; i += 16;
    e.i0 = getbits(b3, i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.crc = getbits(b3, i, 16) * 0x1p-5; i += 16;
    e.omg = getbits(b3, i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.OMGd = getbits(b3, i, 24) * 0x1p-43 * SC_PI; i += 24;
    int iode3 = (int) getbitu(b3, i, 8); i += 8;
    e.idot = getbits(b3, i, 14) * 0x1p-43 * SC_PI;
    // I tre sottoframe devono essere della stessa effemeride.
    if (iode2 != iode3 || ((iodc0 * 256 + iodc1) & 0xff) != iode2) {
        return;
    }
    e.valid = (svh == 0);
    e.t_dec = s_now_abs;
    s_gps[sv] = e;
}

// Galileo I/NAV (E1-B): pagina pari + dispari, CRC, parole 1-5.
static void decode_inav(int sv, const uint32_t *words)
{
    if (sv < 1 || sv > GAL_N) {
        return;
    }
    uint8_t p[32];
    for (int i = 0; i < 8; i++) {
        p[i * 4] = (uint8_t) (words[i] >> 24);
        p[i * 4 + 1] = (uint8_t) (words[i] >> 16);
        p[i * 4 + 2] = (uint8_t) (words[i] >> 8);
        p[i * 4 + 3] = (uint8_t) words[i];
    }
    if (getbitu(p, 1, 1) || getbitu(p, 129, 1)) {
        return; // pagine di allarme
    }
    if (getbitu(p, 0, 1) != 0 || getbitu(p, 128, 1) != 1) {
        return; // serve la coppia pari + dispari
    }
    uint8_t cb[25] = { 0 };
    for (int i = 0; i < 114; i++) {
        setbit(cb, 4 + i, (int) getbitu(p, i, 1));
    }
    for (int i = 0; i < 82; i++) {
        setbit(cb, 118 + i, (int) getbitu(p, 128 + i, 1));
    }
    if (crc24q(cb, 25) != getbitu(p, 128 + 82, 24)) {
        return;
    }
    uint8_t d[16] = { 0 };
    for (int i = 0; i < 112; i++) {
        setbit(d, i, (int) getbitu(p, 2 + i, 1));
    }
    for (int i = 0; i < 16; i++) {
        setbit(d, 112 + i, (int) getbitu(p, 130 + i, 1));
    }
    int type = (int) getbitu(d, 0, 6);
    if (type < 1 || type > 5) {
        return;
    }
    memcpy(s_gal_word[sv][type - 1], d, 16);
    s_gal_word_ok[sv] |= (uint8_t) (1u << (type - 1));
    if (s_gal_word_ok[sv] != 0x1f) {
        return;
    }
    uint8_t (*w)[16] = s_gal_word[sv];
    uint32_t iod = getbitu(w[0], 6, 10);
    for (int t = 1; t < 4; t++) {
        if (getbitu(w[t], 6, 10) != iod) {
            return;
        }
    }
    eph_t e = { .mu = MU_GAL };
    int i = 16;
    e.toe = getbitu(w[0], i, 14) * 60.0; i += 14;
    e.M0 = getbits(w[0], i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.e = getbitu(w[0], i, 32) * 0x1p-33; i += 32;
    e.sqrtA = getbitu(w[0], i, 32) * 0x1p-19;
    i = 16;
    e.OMG0 = getbits(w[1], i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.i0 = getbits(w[1], i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.omg = getbits(w[1], i, 32) * 0x1p-31 * SC_PI; i += 32;
    e.idot = getbits(w[1], i, 14) * 0x1p-43 * SC_PI;
    i = 16;
    e.OMGd = getbits(w[2], i, 24) * 0x1p-43 * SC_PI; i += 24;
    e.deln = getbits(w[2], i, 16) * 0x1p-43 * SC_PI; i += 16;
    e.cuc = getbits(w[2], i, 16) * 0x1p-29; i += 16;
    e.cus = getbits(w[2], i, 16) * 0x1p-29; i += 16;
    e.crc = getbits(w[2], i, 16) * 0x1p-5; i += 16;
    e.crs = getbits(w[2], i, 16) * 0x1p-5;
    i = 16;
    int svid = (int) getbitu(w[3], i, 6); i += 6;
    e.cic = getbits(w[3], i, 16) * 0x1p-29; i += 16;
    e.cis = getbits(w[3], i, 16) * 0x1p-29; i += 16;
    e.toc = getbitu(w[3], i, 14) * 60.0; i += 14;
    e.f0 = getbits(w[3], i, 31) * 0x1p-34; i += 31;
    e.f1 = getbits(w[3], i, 21) * 0x1p-46; i += 21;
    e.f2 = getbits(w[3], i, 6) * 0x1p-59;
    i = 6 + 11 + 11 + 14 + 5 + 10;                // salta ionosfera, regioni e BGD E1-E5a
    e.tgd = getbits(w[4], i, 10) * 0x1p-32; i += 10; // BGD E1-E5b (I/NAV)
    int health = (int) getbitu(w[4], i, 6);          // E5b HS, E1-B HS, DVS
    if (svid != sv) {
        return;
    }
    e.valid = (health == 0);
    e.t_dec = s_now_abs;
    s_gal[sv] = e;
}

// =============================================================================
// Orbite, correzioni, minimi quadrati
static double dt_wrap(double t)
{
    if (t > 302400.0) {
        t -= 604800.0;
    } else if (t < -302400.0) {
        t += 604800.0;
    }
    return t;
}

static bool sat_pos(const eph_t *e, double t, double xs[3], double *dts)
{
    double A = e->sqrtA * e->sqrtA;
    double tk = dt_wrap(t - e->toe);
    if (fabs(tk) > 4 * 3600.0 || A < 1e7) {
        return false;
    }
    double n = sqrt(e->mu / (A * A * A)) + e->deln;
    double M = e->M0 + n * tk, E = M;
    for (int k = 0; k < 30; k++) {
        double En = M + e->e * sin(E);
        if (fabs(En - E) < 1e-13) {
            E = En;
            break;
        }
        E = En;
    }
    double sinE = sin(E), cosE = cos(E);
    double phi = atan2(sqrt(1.0 - e->e * e->e) * sinE, cosE - e->e) + e->omg;
    double s2 = sin(2 * phi), c2 = cos(2 * phi);
    double u = phi + e->cus * s2 + e->cuc * c2;
    double r = A * (1.0 - e->e * cosE) + e->crs * s2 + e->crc * c2;
    double inc = e->i0 + e->idot * tk + e->cis * s2 + e->cic * c2;
    double x = r * cos(u), y = r * sin(u);
    double O = e->OMG0 + (e->OMGd - OMGE) * tk - OMGE * e->toe;
    double cO = cos(O), sO = sin(O), ci = cos(inc);
    xs[0] = x * cO - y * ci * sO;
    xs[1] = x * sO + y * ci * cO;
    xs[2] = y * sin(inc);
    double tc = dt_wrap(t - e->toc);
    *dts = e->f0 + e->f1 * tc + e->f2 * tc * tc + F_REL * e->e * e->sqrtA * sinE;
    return true;
}

static void ecef2geo(const double p[3], double geo[3])
{
    double r = hypot(p[0], p[1]);
    double lat = atan2(p[2], r * (1.0 - WGS_E2)), h = 0;
    for (int k = 0; k < 10; k++) {
        double s = sin(lat);
        double N = WGS_A / sqrt(1.0 - WGS_E2 * s * s);
        h = r / cos(lat) - N;
        lat = atan2(p[2], r * (1.0 - WGS_E2 * N / (N + h)));
    }
    geo[0] = lat;
    geo[1] = atan2(p[1], p[0]);
    geo[2] = h;
}

static void geo2ecef(const double geo[3], double p[3])
{
    double s = sin(geo[0]), c = cos(geo[0]);
    double N = WGS_A / sqrt(1.0 - WGS_E2 * s * s);
    p[0] = (N + geo[2]) * c * cos(geo[1]);
    p[1] = (N + geo[2]) * c * sin(geo[1]);
    p[2] = (N * (1.0 - WGS_E2) + geo[2]) * s;
}

static void to_enu(const double geo[3], const double d[3], double out[3])
{
    double sl = sin(geo[0]), cl = cos(geo[0]), so = sin(geo[1]), co = cos(geo[1]);
    out[0] = -so * d[0] + co * d[1];
    out[1] = -sl * co * d[0] - sl * so * d[1] + cl * d[2];
    out[2] = cl * co * d[0] + cl * so * d[1] + sl * d[2];
}

// Saastamoinen con atmosfera standard.
static double tropo(double h, double el)
{
    if (h < -100.0 || h > 1e4 || el <= 0) {
        return 0;
    }
    double hh = h < 0 ? 0 : h;
    double P = 1013.25 * pow(1.0 - 2.2557e-5 * hh, 5.2568);
    double T = 15.0 - 6.5e-3 * hh + 273.16;
    double e = 6.108 * 0.7 * exp((17.15 * T - 4684.0) / (T - 38.45));
    return 0.0022768 * (P + (1255.0 / T + 0.05) * e) / cos(SC_PI / 2 - el);
}

// Klobuchar (IS-GPS-200): ritardo sulla L1 in metri. Usato anche per Galileo E1
// (stessa frequenza): il residuo e' di qualche metro, poi la mediana lo smorza.
static double klobuchar(double t, const double geo[3], double az, double el)
{
    if (!s_ion_ok || el <= 0) {
        return 0;
    }
    double psi = 0.0137 / (el / SC_PI + 0.11) - 0.022;
    double phi = geo[0] / SC_PI + psi * cos(az);
    if (phi > 0.416) {
        phi = 0.416;
    } else if (phi < -0.416) {
        phi = -0.416;
    }
    double lam = geo[1] / SC_PI + psi * sin(az) / cos(phi * SC_PI);
    double phim = phi + 0.064 * cos((lam - 1.617) * SC_PI);
    double tt = 43200.0 * lam + t;
    tt -= floor(tt / 86400.0) * 86400.0;
    double f = 1.0 + 16.0 * pow(0.53 - el / SC_PI, 3);
    double amp = s_ion[0] + phim * (s_ion[1] + phim * (s_ion[2] + phim * s_ion[3]));
    double per = s_ion[4] + phim * (s_ion[5] + phim * (s_ion[6] + phim * s_ion[7]));
    if (amp < 0) {
        amp = 0;
    }
    if (per < 72000.0) {
        per = 72000.0;
    }
    double x = 2.0 * SC_PI * (tt - 50400.0) / per;
    return CLIGHT * f * (fabs(x) < 1.57 ? 5e-9 + amp * (1.0 + x * x * (-0.5 + x * x / 24.0)) : 5e-9);
}

// Risolve N x = u (n <= 5), Gauss con pivot parziale.
static bool solve(double N[5][6], int n, double *x)
{
    for (int c = 0; c < n; c++) {
        int p = c;
        for (int r = c + 1; r < n; r++) {
            if (fabs(N[r][c]) > fabs(N[p][c])) {
                p = r;
            }
        }
        if (p != c) {
            for (int k = 0; k <= n; k++) {
                double t = N[c][k];
                N[c][k] = N[p][k];
                N[p][k] = t;
            }
        }
        if (fabs(N[c][c]) < 1e-9) {
            return false;
        }
        for (int r = 0; r < n; r++) {
            if (r == c) {
                continue;
            }
            double f = N[r][c] / N[c][c];
            for (int k = c; k <= n; k++) {
                N[r][k] -= f * N[c][k];
            }
        }
    }
    for (int i = 0; i < n; i++) {
        x[i] = N[i][n] / N[i][i];
    }
    return true;
}

// Posizione singola L1/E1 per un'epoca RAWX. Ritorna il numero di
// satelliti usati (0 = nessuna soluzione), posizione in out[3].
static int spp(double tow, int nobs, double out[3])
{
    bool has_gps = false, has_gal = false;
    for (int i = 0; i < nobs; i++) {
        s_obs[i].reject = false;
        if (s_obs[i].gal) {
            has_gal = true;
        } else {
            has_gps = true;
        }
    }
    // Incognite: x, y, z, orologio GPS (indice 3), orologio Galileo (indice 4 o 3).
    int nx = 3 + (has_gps ? 1 : 0) + (has_gal ? 1 : 0);
    int gal_idx = has_gps ? 4 : 3;
    if (nobs < nx + 1) {
        return 0;
    }
    double x[5] = { 0 };
    if (s_x_ok) {
        x[0] = s_x[0];
        x[1] = s_x[1];
        x[2] = s_x[2];
        x[3] = has_gps ? s_x[3] : s_x[4];
        x[4] = s_x[4];
    }
    int used = 0;
    double sumsq = 0;
    for (int iter = 0; iter < 12; iter++) {
        double N[5][6] = { { 0 } };
        double geo[3];
        ecef2geo(x, geo);
        bool first = sqrt(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]) < 6e6;
        used = 0;
        sumsq = 0;
        int worst = -1;
        double worst_v = 0, worst_res = 0;
        for (int i = 0; i < nobs; i++) {
            obs_t *o = &s_obs[i];
            if (o->reject) {
                continue;
            }
            double xs[3], dts;
            if (!sat_pos(o->e, tow - o->pr / CLIGHT, xs, &dts)) {
                continue;
            }
            double rho = sqrt((xs[0] - x[0]) * (xs[0] - x[0]) + (xs[1] - x[1]) * (xs[1] - x[1]) + (xs[2] - x[2]) * (xs[2] - x[2]));
            double rot = OMGE * rho / CLIGHT; // rotazione terrestre durante il volo del segnale
            double sx = xs[0] * cos(rot) + xs[1] * sin(rot);
            double sy = -xs[0] * sin(rot) + xs[1] * cos(rot);
            double sz = xs[2];
            rho = sqrt((sx - x[0]) * (sx - x[0]) + (sy - x[1]) * (sy - x[1]) + (sz - x[2]) * (sz - x[2]));
            double w = 1.0, corr = 0;
            if (!first) {
                double d[3] = { sx - x[0], sy - x[1], sz - x[2] }, enu[3];
                to_enu(geo, d, enu);
                double el = asin(enu[2] / rho), az = atan2(enu[0], enu[1]);
                if (el < ELMASK_DEG * SC_PI / 180.0) {
                    continue;
                }
                w = sin(el) * sin(el);
                corr = tropo(geo[2], el) + klobuchar(tow, geo, az, el);
            }
            int ci = o->gal ? gal_idx : 3;
            double res = o->pr - (rho + x[ci] - CLIGHT * (dts - o->e->tgd) + corr);
            double H[5] = { 0 };
            H[0] = (x[0] - sx) / rho;
            H[1] = (x[1] - sy) / rho;
            H[2] = (x[2] - sz) / rho;
            H[ci] = 1.0;
            for (int r = 0; r < nx; r++) {
                N[r][nx] += w * H[r] * res;
                for (int c = 0; c < nx; c++) {
                    N[r][c] += w * H[r] * H[c];
                }
            }
            used++;
            sumsq += res * res;
            if (fabs(res) * sqrt(w) > worst_v) {
                worst_v = fabs(res) * sqrt(w);
                worst_res = fabs(res);
                worst = i;
            }
        }
        if (used < nx + 1) {
            return 0;
        }
        double dx[5];
        if (!solve(N, nx, dx)) {
            return 0;
        }
        for (int k = 0; k < nx; k++) {
            x[k] += dx[k];
        }
        if (sqrt(dx[0] * dx[0] + dx[1] * dx[1] + dx[2] * dx[2]) < 1e-3) {
            // Convergenza: scarta il satellite peggiore se molto fuori e ricalcola.
            if (!first && worst >= 0 && used > nx + 1 && worst_res > 15.0) {
                s_obs[worst].reject = true;
                continue;
            }
            break;
        }
    }
    if (used == 0 || sqrt(sumsq / used) > 10.0) {
        return 0;
    }
    s_x[0] = x[0];
    s_x[1] = x[1];
    s_x[2] = x[2];
    if (has_gps) {
        s_x[3] = x[3];
    }
    if (has_gal) {
        s_x[4] = x[gal_idx];
    }
    s_x_ok = true;
    out[0] = x[0];
    out[1] = x[1];
    out[2] = x[2];
    return used;
}

// =============================================================================
// Finestre e confronto con la posizione trasmessa
static int cmp_float(const void *a, const void *b)
{
    float fa = *(const float *) a, fb = *(const float *) b;
    return (fa > fb) - (fa < fb);
}

static float median(const float *v, int n)
{
    memcpy(s_sort, v, (size_t) n * sizeof(float));
    qsort(s_sort, (size_t) n, sizeof(float), cmp_float);
    return (n & 1) ? s_sort[n / 2] : 0.5f * (s_sort[n / 2 - 1] + s_sort[n / 2]);
}

static double s_ref[3];       // posizione trasmessa (1005), riportata in ITRF2020/WGS84
static double s_ref_raw[3];   // come arriva nel 1005, per accorgersi se cambia
static bool s_ref_ok;
static int64_t s_win_start_us;
static int64_t s_result_us;
static double s_last_tow = -1;

// Il 1005 delle coordinate fisse e' in ETRF2000 (sistema della rete), la
// posizione calcolata qui in ITRF2020 (orbite trasmesse, WGS84): in Italia
// circa 1 m di differenza, che si toglie per non falsare il confronto.
static void ref_to_itrf(const double raw[3], double out[3])
{
    out[0] = raw[0];
    out[1] = raw[1];
    out[2] = raw[2];
    if (!s_fixed_etrf) {
        return;
    }
    time_t now = time(NULL);
    double epoch = etrf_decimal_year(now);
    if (epoch < 2024.0) {
        epoch = 2026.8; // ora non ancora sincronizzata: l'errore e' di pochi millimetri
    }
    double geo[3];
    ecef2geo(raw, geo);
    double lat = geo[0] * 180.0 / SC_PI, lon = geo[1] * 180.0 / SC_PI, h = geo[2];
    etrf_itrf2020_to_etrf2000(&lat, &lon, &h, epoch);
    double g2[3] = { lat * SC_PI / 180.0, lon * SC_PI / 180.0, h }, fwd[3];
    geo2ecef(g2, fwd);
    for (int k = 0; k < 3; k++) {
        out[k] = raw[k] - (fwd[k] - raw[k]);
    }
}

static void reset_window(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_st.samples = 0;
    s_st.window_elapsed_s = 0;
    xSemaphoreGive(s_mutex);
    s_win_start_us = esp_timer_get_time();
}

static void close_window(void)
{
    int n = s_st.samples;
    if (n < MIN_SAMPLES) {
        ESP_LOGI(TAG, "Finestra di 30 min con solo %d soluzioni: scartata", n);
        reset_window();
        return;
    }
    float de = median(s_se, n), dn = median(s_sn, n), du = median(s_su, n);
    float hz = sqrtf(de * de + dn * dn);
    settings_get_into(s_cfg);
    float thr = s_cfg->base_drift_threshold_m > 0 ? s_cfg->base_drift_threshold_m : 5.0f;
    bool over = hz >= thr || fabsf(du) >= 2.0f * thr;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_st.have_result = true;
    s_st.d_east_m = de;
    s_st.d_north_m = dn;
    s_st.d_up_m = du;
    s_st.horiz_m = hz;
    s_st.result_samples = (uint16_t) n;
    s_st.windows++;
    s_st.exceed = over ? (uint8_t) (s_st.exceed < 255 ? s_st.exceed + 1 : 255) : 0;
    s_st.alarm = s_st.exceed >= ALARM_WINDOWS;
    s_st.threshold_m = thr;
    xSemaphoreGive(s_mutex);
    s_result_us = esp_timer_get_time();
    ESP_LOGI(TAG, "Posizione calcolata vs trasmessa (mediana di %d soluzioni): E %.2f N %.2f U %.2f m, orizz. %.2f m%s",
             n, de, dn, du, hz, over ? " - OLTRE LA SOGLIA" : "");
    reset_window();
}

// Effemeride sana e decodificata da meno di EPH_MAX_AGE_S. Quelle decodificate
// prima del primo RAWX (tempo ancora ignoto) prendono il tempo di adesso.
static bool eph_usable(eph_t *e)
{
    if (!e->valid) {
        return false;
    }
    if (e->t_dec <= 0) {
        e->t_dec = s_now_abs;
    }
    return s_now_abs - e->t_dec <= EPH_MAX_AGE_S;
}

static void process_rawx(const uint8_t *p, size_t len)
{
    if (len < 16) {
        return;
    }
    double tow;
    memcpy(&tow, p, 8);
    int nm = p[11];
    if ((size_t) (16 + 32 * nm) > len) {
        return;
    }
    // Tempo assoluto (settimana GPS @8) per l'eta' delle effemeridi.
    s_now_abs = ((uint32_t) p[8] | ((uint32_t) p[9] << 8)) * 604800.0 + tow;
    // Una soluzione ogni ~5 s anche se la registrazione dei dati grezzi chiede 1 s.
    if (s_last_tow >= 0 && fabs(dt_wrap(tow - s_last_tow)) < 4.5) {
        return;
    }
    s_last_tow = tow;

    int ng = 0, ne = 0;
    for (int i = 1; i <= GPS_N; i++) {
        ng += eph_usable(&s_gps[i]);
    }
    for (int i = 1; i <= GAL_N; i++) {
        ne += eph_usable(&s_gal[i]);
    }
    int nobs = 0;
    for (int k = 0; k < nm && nobs < MAX_OBS; k++) {
        const uint8_t *q = p + 16 + 32 * k;
        uint8_t gnss = q[20], sv = q[21], sig = q[22], cno = q[26], trk = q[30];
        if (!(trk & 1) || cno < CNO_MIN) {
            continue;
        }
        const eph_t *e = NULL;
        bool gal = false;
        if (gnss == 0 && sig == 0 && sv >= 1 && sv <= GPS_N) {
            e = &s_gps[sv];
        } else if (gnss == 2 && (sig == 0 || sig == 1) && sv >= 1 && sv <= GAL_N) {
            e = &s_gal[sv];
            gal = true;
        }
        if (!e || !eph_usable((eph_t *) e)) {
            continue;
        }
        double pr;
        memcpy(&pr, q, 8);
        if (pr < 1.8e7 || pr > 3.2e7) {
            continue;
        }
        bool dup = false;
        for (int j = 0; j < nobs; j++) {
            if (s_obs[j].e == e) {
                dup = true; // E1-B ed E1-C dello stesso satellite: ne basta una
                break;
            }
        }
        if (dup) {
            continue;
        }
        s_obs[nobs].e = e;
        s_obs[nobs].pr = pr;
        s_obs[nobs].gal = gal;
        nobs++;
    }

    double pos[3];
    int used = spp(tow, nobs, pos);

    // Posizione trasmessa (1005): se cambia (nuovo survey-in, coordinate
    // nuove) si ricomincia da capo.
    base_monitor_status_t bm = base_monitor_get_status();
    bool ref_now = bm.last_position_set;
    if (ref_now) {
        double raw[3] = { bm.last_ecef_x_m, bm.last_ecef_y_m, bm.last_ecef_z_m };
        double d = sqrt((raw[0] - s_ref_raw[0]) * (raw[0] - s_ref_raw[0]) + (raw[1] - s_ref_raw[1]) * (raw[1] - s_ref_raw[1]) +
                        (raw[2] - s_ref_raw[2]) * (raw[2] - s_ref_raw[2]));
        if (!s_ref_ok || d > 0.05) {
            memcpy(s_ref_raw, raw, sizeof(raw));
            ref_to_itrf(raw, s_ref);
            if (s_ref_ok) {
                ESP_LOGI(TAG, "Posizione trasmessa cambiata (%.2f m): confronto ricominciato", d);
            }
            s_ref_ok = true;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_st.exceed = 0;
            s_st.alarm = false;
            s_st.have_result = false;
            xSemaphoreGive(s_mutex);
            reset_window();
        }
    }

    uint8_t state = !ref_now ? BASE_SELFPOS_WAIT_1005 : (used ? BASE_SELFPOS_RUNNING : BASE_SELFPOS_WAIT_SATS);
    if (!ref_now && !used) {
        state = BASE_SELFPOS_WAIT_SATS;
    }
    if (used && ref_now && s_st.samples < MAX_SAMPLES) {
        double geo[3], d[3] = { pos[0] - s_ref[0], pos[1] - s_ref[1], pos[2] - s_ref[2] }, enu[3];
        ecef2geo(s_ref, geo);
        to_enu(geo, d, enu);
        // Oltre 10 km e' una soluzione sbagliata, non uno spostamento.
        if (fabs(enu[0]) < 1e4 && fabs(enu[1]) < 1e4 && fabs(enu[2]) < 1e4) {
            s_se[s_st.samples] = (float) enu[0];
            s_sn[s_st.samples] = (float) enu[1];
            s_su[s_st.samples] = (float) enu[2];
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_st.samples++;
            xSemaphoreGive(s_mutex);
        }
    }
    int64_t now = esp_timer_get_time();
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_st.state = state;
    s_st.sats = (uint8_t) used;
    s_st.eph_gps = (uint8_t) ng;
    s_st.eph_gal = (uint8_t) ne;
    s_st.window_elapsed_s = ref_now ? (uint32_t) ((now - s_win_start_us) / 1000000) : 0;
    xSemaphoreGive(s_mutex);
    if (ref_now && now - s_win_start_us >= (int64_t) WINDOW_S * 1000000) {
        close_window();
    }
}

static void process_frame(const uint8_t *f, size_t n)
{
    size_t plen = (size_t) f[4] | ((size_t) f[5] << 8);
    const uint8_t *p = f + 6;
    if (n < 8 + plen) {
        return;
    }
    if (f[3] == 0x13) {
        if (plen < 8) {
            return;
        }
        int gnss = p[0], sv = p[1], nw = p[4];
        if ((size_t) (8 + 4 * nw) > plen || nw > 16) {
            return;
        }
        uint32_t w[16];
        for (int k = 0; k < nw; k++) {
            const uint8_t *q = p + 8 + 4 * k;
            w[k] = (uint32_t) q[0] | ((uint32_t) q[1] << 8) | ((uint32_t) q[2] << 16) | ((uint32_t) q[3] << 24);
        }
        if (gnss == 0 && nw >= 10) {
            decode_lnav(sv, w);
        } else if (gnss == 2 && nw >= 8) {
            decode_inav(sv, w);
        }
    } else if (f[3] == 0x15) {
        process_rawx(p, plen);
    }
}

static void selfpos_task(void *arg)
{
    reset_window();
    while (1) {
        size_t n = xMessageBufferReceive(s_queue, s_work, 6 + MAX_PAYLOAD + 2, pdMS_TO_TICKS(60000));
        if (n >= 8) {
            process_frame(s_work, n);
        }
        if (n == 0) {
            // Nessun dato per un minuto: ricevitore fermo o uscita spenta.
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_st.state = BASE_SELFPOS_WAIT_SATS;
            s_st.sats = 0;
            xSemaphoreGive(s_mutex);
        }
        if (s_result_us > 0) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_st.result_age_s = (uint32_t) ((esp_timer_get_time() - s_result_us) / 1000000);
            xSemaphoreGive(s_mutex);
        }
        static uint32_t loops;
        if (++loops == 60 || loops % 2000 == 0) {
            ESP_LOGI(TAG, "Stack libero minimo %u byte", (unsigned) uxTaskGetStackHighWaterMark(NULL));
        }
    }
}

// =============================================================================
void base_selfpos_start(bool ublox_base, bool fixed_coords_etrf2000)
{
    s_mutex = xSemaphoreCreateMutex();
    s_st.window_len_s = WINDOW_S;
    if (!ublox_base) {
        s_st.state = BASE_SELFPOS_OFF;
        return;
    }
    s_fixed_etrf = fixed_coords_etrf2000;
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    uint8_t *qmem = heap_caps_malloc(QUEUE_BYTES, caps);
    StaticMessageBuffer_t *qctl = heap_caps_malloc(sizeof(StaticMessageBuffer_t), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
    s_frame = heap_caps_malloc(6 + MAX_PAYLOAD + 2, caps);
    s_work = heap_caps_malloc(6 + MAX_PAYLOAD + 2, caps);
    s_se = heap_caps_malloc(MAX_SAMPLES * sizeof(float), caps);
    s_sn = heap_caps_malloc(MAX_SAMPLES * sizeof(float), caps);
    s_su = heap_caps_malloc(MAX_SAMPLES * sizeof(float), caps);
    s_sort = heap_caps_malloc(MAX_SAMPLES * sizeof(float), caps);
    s_gps = heap_caps_calloc(GPS_N + 1, sizeof(eph_t), caps);
    s_gal = heap_caps_calloc(GAL_N + 1, sizeof(eph_t), caps);
    s_gps_sub = heap_caps_calloc(GPS_N + 1, sizeof(*s_gps_sub), caps);
    s_gal_word = heap_caps_calloc(GAL_N + 1, sizeof(*s_gal_word), caps);
    s_obs = heap_caps_calloc(MAX_OBS, sizeof(obs_t), caps);
    s_cfg = heap_caps_calloc(1, sizeof(app_settings_t), caps);
    if (!qmem || !qctl || !s_frame || !s_work || !s_se || !s_sn || !s_su || !s_sort ||
        !s_gps || !s_gal || !s_gps_sub || !s_gal_word || !s_obs || !s_cfg) {
        ESP_LOGE(TAG, "Memoria non disponibile: controllo dello spostamento disattivato");
        s_st.state = BASE_SELFPOS_OFF;
        return;
    }
    s_queue = xMessageBufferCreateStatic(QUEUE_BYTES, qmem, qctl);
    s_st.state = BASE_SELFPOS_WAIT_SATS;
    s_st.threshold_m = 5.0f;
    // 6 KB: solo variabili semplici sullo stack, tutto il resto e' statico.
    // Stack in PSRAM (RAM interna scarsa): il task non scrive mai in flash/NVS.
    if (xTaskCreateWithCaps(selfpos_task, "base_selfpos", 6144, NULL, 3, NULL, MALLOC_CAP_SPIRAM) != pdPASS &&
        xTaskCreate(selfpos_task, "base_selfpos", 6144, NULL, 3, NULL) != pdPASS) {
        s_st.state = BASE_SELFPOS_OFF;
        return;
    }
    s_enabled = true;
    ESP_LOGI(TAG, "Controllo dello spostamento dell'antenna attivo (coordinate %s)",
             fixed_coords_etrf2000 ? "fisse ETRF2000" : "del survey-in");
}

uint8_t base_selfpos_wanted_rate(void)
{
    return s_enabled ? RAWX_RATE : 0;
}

void base_selfpos_feed(const uint8_t *buf, size_t len)
{
    if (!s_enabled) {
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (s_pos == 0) {
            if (c == 0xB5) {
                s_frame[s_pos++] = c;
            }
            continue;
        }
        if (s_pos == 1) {
            if (c == 0x62) {
                s_frame[s_pos++] = c;
            } else {
                s_pos = (c == 0xB5) ? 1 : 0;
            }
            continue;
        }
        s_frame[s_pos++] = c;
        if (s_pos == 6) {
            size_t plen = (size_t) s_frame[4] | ((size_t) s_frame[5] << 8);
            bool wanted = s_frame[2] == 0x02 && (s_frame[3] == 0x15 || s_frame[3] == 0x13);
            if (!wanted || plen > MAX_PAYLOAD) {
                s_pos = 0;
                continue;
            }
            s_need = 6 + plen + 2;
        }
        if (s_pos >= 6 && s_pos == s_need) {
            uint8_t a = 0, b = 0;
            for (size_t k = 2; k < s_need - 2; k++) {
                a += s_frame[k];
                b += a;
            }
            if (a == s_frame[s_need - 2] && b == s_frame[s_need - 1]) {
                // Coda piena (task in ritardo): il frame si perde, nessuna attesa qui.
                xMessageBufferSend(s_queue, s_frame, s_need, 0);
            }
            s_pos = 0;
        }
    }
}

void base_selfpos_get_status(base_selfpos_status_t *out)
{
    if (!s_mutex) {
        memset(out, 0, sizeof(*out));
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    *out = s_st;
    xSemaphoreGive(s_mutex);
}
