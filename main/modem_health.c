// Salute del modem SIM7600 (vedi modem_health.h). Formati dal manuale
// "SIM7500_SIM7600 Series AT Command Manual":
//   +CPMUTEMP: 36
//   +CBC: 3.801V
//   +CPSI: LTE,Online,222-88,0x5A1E,187214780,257,EUTRAN-BAND20,6300,5,5,-94,-850,-545,15
//          (RSRQ, RSRP e RSSI in decimi; RSSNR in dB)
//   +CGMR: LE20B04SIM7600M22
#include "modem_health.h"

#include <stdlib.h>
#include <string.h>

// Testo subito dopo "<chiave>: " nella risposta (che puo' contenere l'eco del
// comando e righe vuote), NULL se manca.
static const char *after_key(const char *resp, const char *key)
{
    if (!resp) return NULL;
    const char *p = strstr(resp, key);
    if (!p) return NULL;
    p += strlen(key);
    while (*p == ' ') p++;
    return p;
}

// Intero con segno senza caratteri estranei fino a ',' o fine campo.
static bool to_int(const char *s, size_t len, long *v)
{
    if (len == 0 || len > 11) return false;
    char buf[12];
    memcpy(buf, s, len);
    buf[len] = '\0';
    char *end;
    long x = strtol(buf, &end, 10);
    if (end == buf || *end != '\0') return false;
    *v = x;
    return true;
}

bool modem_health_parse_temp(const char *resp, int *temp_c)
{
    const char *p = after_key(resp, "+CPMUTEMP:");
    if (!p) return false;
    size_t n = strspn(p, "-0123456789");
    long v;
    if (!to_int(p, n, &v) || v < -40 || v > 125) return false;
    *temp_c = (int) v;
    return true;
}

bool modem_health_parse_cbc(const char *resp, int *volt_mv)
{
    const char *p = after_key(resp, "+CBC:");
    if (!p) return false;
    // Alcuni firmware rispondono "<bcs>,<bcl>,<tensione>": conta l'ultimo campo.
    const char *last = p;
    for (const char *q = p; *q && *q != '\r' && *q != '\n'; q++) {
        if (*q == ',') last = q + 1;
    }
    char *end;
    double v = strtod(last, &end);
    if (end == last) return false;
    int mv = (int) (v < 100 ? v * 1000.0 + 0.5 : v + 0.5);  // "3.801V" oppure "3801"
    if (mv < 2500 || mv > 5500) return false;
    *volt_mv = mv;
    return true;
}

// Valori in decimi se fuori scala ("-850" = -85,0 dBm).
static float tenths(long v, long max_abs)
{
    return (v > max_abs || v < -max_abs) ? (float) v / 10.0f : (float) v;
}

bool modem_health_parse_cpsi(const char *resp, modem_health_t *h)
{
    const char *p = after_key(resp, "+CPSI:");
    if (!p) return false;
    const char *f[20];
    size_t len[20];
    int n = 0;
    const char *s = p;
    for (;;) {
        const char *e = s;
        while (*e && *e != ',' && *e != '\r' && *e != '\n') e++;
        if (n < 20) { f[n] = s; len[n] = (size_t) (e - s); n++; }
        if (*e != ',') break;
        s = e + 1;
    }
    size_t ml = len[0] < sizeof(h->mode) - 1 ? len[0] : sizeof(h->mode) - 1;
    memcpy(h->mode, f[0], ml);
    h->mode[ml] = '\0';
    h->lte_ok = false;
    if (strcmp(h->mode, "LTE") != 0 || n < 14) return true;

    long rsrq, rsrp, rssi, sinr;
    if (!to_int(f[10], len[10], &rsrq) || !to_int(f[11], len[11], &rsrp) ||
        !to_int(f[12], len[12], &rssi) || !to_int(f[13], len[13], &sinr)) {
        return true;
    }
    h->rsrq_db = tenths(rsrq, 30);
    h->rsrp_dbm = tenths(rsrp, 150);
    h->rssi_dbm = tenths(rssi, 120);
    h->sinr_db = (int) sinr;
    if (h->rsrp_dbm < -150 || h->rsrp_dbm > -30 || h->rsrq_db < -30 || h->rsrq_db > 0) return true;

    size_t cl = len[2] < sizeof(h->mcc_mnc) - 1 ? len[2] : sizeof(h->mcc_mnc) - 1;
    memcpy(h->mcc_mnc, f[2], cl);
    h->mcc_mnc[cl] = '\0';
    h->cell_id = strtoul(f[4], NULL, 10);
    h->band = 0;
    const char *b = f[6];
    const char *k = "EUTRAN-BAND";
    if (len[6] > strlen(k) && strncmp(b, k, strlen(k)) == 0) {
        long bn;
        if (to_int(b + strlen(k), len[6] - strlen(k), &bn) && bn > 0 && bn < 256) h->band = (int) bn;
    }
    h->lte_ok = true;
    return true;
}

bool modem_health_parse_cgmr(const char *resp, char *out, size_t out_size)
{
    const char *p = after_key(resp, "+CGMR:");
    if (!p || out_size < 2) return false;
    size_t n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && n < out_size - 1) n++;
    while (n > 0 && p[n - 1] == ' ') n--;
    if (n == 0) return false;
    memcpy(out, p, n);
    out[n] = '\0';
    return true;
}
