#include "nmea_etrf.h"
#include "etrf.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define MAX_FIELDS 24
#define LINE_MAX   200

// Campi separati da virgola, fino all'asterisco del checksum (escluso).
// I puntatori indicano dentro buf, le virgole sono sostituite da '\0'.
static int split(char *buf, char **f)
{
    char *star = strchr(buf, '*');
    if (star) {
        *star = '\0';
    }
    int n = 0;
    f[n++] = buf;
    for (char *p = buf; *p && n < MAX_FIELDS; p++) {
        if (*p == ',') {
            *p = '\0';
            f[n++] = p + 1;
        }
    }
    return n;
}

static int decimals(const char *s)
{
    const char *dot = strchr(s, '.');
    return dot ? (int) strlen(dot + 1) : 0;
}

// "4049.123456","N" -> gradi decimali.
static bool parse_coord(const char *v, const char *hemi, double *deg)
{
    if (!v[0] || !hemi[0]) {
        return false;
    }
    double x = atof(v);
    int d = (int) (x / 100);
    *deg = d + (x - d * 100) / 60.0;
    if (hemi[0] == 'S' || hemi[0] == 'W') {
        *deg = -*deg;
    }
    return true;
}

// Gradi decimali -> "ddmm.mmmm" (lat, 2 cifre di gradi) o "dddmm.mmmm" (lon),
// con dec decimali sui minuti.
static void format_coord(char *out, size_t size, double deg, int deg_digits, int dec)
{
    double a = fabs(deg);
    int d = (int) a;
    double m = (a - d) * 60.0;
    double scale = pow(10, dec);
    m = round(m * scale) / scale;
    if (m >= 60.0) { // arrotondamento a 60,000: riporto sui gradi
        m -= 60.0;
        d++;
    }
    snprintf(out, size, "%0*d%0*.*f", deg_digits, d, dec ? dec + 3 : 2, dec, m);
}

int nmea_gga_quality(const char *line)
{
    static char buf[LINE_MAX]; // statici: solo dal task che legge il ricevitore, stack piccolo
    char *f[MAX_FIELDS];
    strlcpy(buf, line, sizeof(buf));
    int n = split(buf, f);
    if (n < 7 || strlen(f[0]) < 6 || memcmp(f[0] + 3, "GGA", 3) != 0 || !f[6][0]) {
        return -1;
    }
    return atoi(f[6]);
}

bool nmea_gga_ellipsoidal_height(const char *line, double *h_m)
{
    static char buf[LINE_MAX]; // statici: solo dal task che legge il ricevitore, stack piccolo
    char *f[MAX_FIELDS];
    strlcpy(buf, line, sizeof(buf));
    int n = split(buf, f);
    if (n < 12 || memcmp(f[0] + 3, "GGA", 3) != 0 || !f[9][0] || !f[11][0]) {
        return false;
    }
    *h_m = atof(f[9]) + atof(f[11]);
    return true;
}

// Riscrive posizione (e in GGA quota, qualita', eta' delle correzioni) di una
// riga GGA/RMC. given = {lat, lon, quota ellissoidica} da mettere al posto di
// quella della riga (NULL = quella della riga); in ogni caso la posizione e'
// ITRF2020 e viene convertita in ETRF2000. quality < 0 / age < 0: invariati.
static bool rewrite(char *line, size_t cap, double epoch, double h_ell_m,
                    const double *given, int quality, float age)
{
    static char buf[LINE_MAX]; // statici: solo dal task che legge il ricevitore, stack piccolo
    char *f[MAX_FIELDS];
    if (line[0] != '$' || strlen(line) >= sizeof(buf)) {
        return false;
    }
    strlcpy(buf, line, sizeof(buf));
    int n = split(buf, f);
    if (strlen(f[0]) < 6) {
        return false;
    }
    bool gga = memcmp(f[0] + 3, "GGA", 3) == 0;
    bool rmc = memcmp(f[0] + 3, "RMC", 3) == 0;
    int ilat = gga ? 2 : 3; // RMC: 1 ora, 2 stato, 3 lat
    if ((!gga && !rmc) || n < ilat + 4) {
        return false;
    }
    double lat, lon;
    if (!parse_coord(f[ilat], f[ilat + 1], &lat) || !parse_coord(f[ilat + 2], f[ilat + 3], &lon)) {
        return false;
    }
    double h = h_ell_m, sep = 0;
    bool has_h = gga && n >= 12 && f[9][0] && f[11][0];
    if (has_h) {
        sep = atof(f[11]);
        h = atof(f[9]) + sep;
    }
    int dec_lat = decimals(f[ilat]), dec_lon = decimals(f[ilat + 2]), dec_alt = has_h ? decimals(f[9]) : 0;
    if (given) {
        lat = given[0];
        lon = given[1];
        h = given[2];
        // la soluzione HAS ha piu' cifre della GGA autonoma: almeno 7 decimali di minuto (0,2 mm)
        if (dec_lat < 7) dec_lat = 7;
        if (dec_lon < 7) dec_lon = 7;
        if (dec_alt < 3) dec_alt = 3;
    }

    etrf_itrf2020_to_etrf2000(&lat, &lon, &h, epoch);

    char lat_s[24], lon_s[24], alt_s[24], q_s[12], age_s[24];
    format_coord(lat_s, sizeof(lat_s), lat, 2, dec_lat);
    format_coord(lon_s, sizeof(lon_s), lon, 3, dec_lon);
    f[ilat] = lat_s;
    f[ilat + 1] = lat >= 0 ? "N" : "S";
    f[ilat + 2] = lon_s;
    f[ilat + 3] = lon >= 0 ? "E" : "W";
    if (has_h) {
        snprintf(alt_s, sizeof(alt_s), "%.*f", dec_alt, h - sep);
        f[9] = alt_s;
    }
    if (gga && quality >= 0) {
        snprintf(q_s, sizeof(q_s), "%d", quality);
        f[6] = q_s;
    }
    if (gga && age >= 0 && n >= 14) {
        snprintf(age_s, sizeof(age_s), "%.1f", (double) age);
        f[13] = age_s;
    }

    // Ricompone la riga e il checksum (XOR tra '$' e '*').
    static char out[LINE_MAX + 16];
    size_t k = 0;
    for (int i = 0; i < n; i++) {
        int w = snprintf(out + k, sizeof(out) - k, i ? ",%s" : "%s", f[i]);
        if (w < 0 || (size_t) w >= sizeof(out) - k) {
            return false;
        }
        k += (size_t) w;
    }
    unsigned char cs = 0;
    for (size_t i = 1; i < k; i++) {
        cs ^= (unsigned char) out[i];
    }
    int w = snprintf(out + k, sizeof(out) - k, "*%02X", cs);
    if (w < 0 || k + (size_t) w >= cap) {
        return false;
    }
    memcpy(line, out, k + (size_t) w + 1);
    return true;
}

bool nmea_etrf_convert(char *line, size_t cap, double epoch, double h_ell_m)
{
    return rewrite(line, cap, epoch, h_ell_m, NULL, -1, -1);
}

bool nmea_etrf_set_position(char *line, size_t cap, double epoch, double lat_deg, double lon_deg,
                            double h_ell_m, int quality, float age_s)
{
    const double given[3] = { lat_deg, lon_deg, h_ell_m };
    return rewrite(line, cap, epoch, h_ell_m, given, quality, age_s);
}

bool nmea_gga_latlon(const char *line, double *lat_deg, double *lon_deg)
{
    static char buf[LINE_MAX];
    char *f[MAX_FIELDS];
    strlcpy(buf, line, sizeof(buf));
    int n = split(buf, f);
    if (n < 6 || strlen(f[0]) < 6 || memcmp(f[0] + 3, "GGA", 3) != 0) {
        return false;
    }
    return parse_coord(f[2], f[3], lat_deg) && parse_coord(f[4], f[5], lon_deg);
}
