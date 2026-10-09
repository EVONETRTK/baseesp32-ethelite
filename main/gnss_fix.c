#include "gnss_fix.h"

#include <string.h>
#include <math.h>
#include <stdlib.h>

#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static gnss_fix_status_t s_status;
static SemaphoreHandle_t s_mutex;

void gnss_fix_init(void)
{
    memset(&s_status, 0, sizeof(s_status));
    s_status.hdop = -1;
    s_status.altitude_m = -9999;
    s_status.diff_age_s = -1;
    s_status.geoid_sep_m = -9999;
    s_mutex = xSemaphoreCreateMutex();
}

// Isola il prossimo campo separato da virgola in *cursor, terminandolo con
// '\0' (o troncando su '*' per l'ultimo campo prima del checksum). Muta la
// stringa originale - va lavorato su una copia locale. Stessa logica di
// gnss_signal.c, duplicata qui perche' e' privata a quel file.
static char *next_field(char **cursor)
{
    if (!*cursor) {
        return NULL;
    }
    char *start = *cursor;
    char *comma = strchr(start, ',');
    if (comma) {
        *comma = '\0';
        *cursor = comma + 1;
    } else {
        char *star = strchr(start, '*');
        if (star) {
            *star = '\0';
        }
        *cursor = NULL;
    }
    return start;
}

// "ddmm.mmmmmmm" / "dddmm.mmmmmmm" -> gradi decimali (strtod per non
// perdere le cifre della modalita' NMEA ad alta precisione).
static double nmea_coord_deg(const char *field, bool negative)
{
    double v = strtod(field, NULL);
    // Riga rovinata (trovato dal fuzzing): fuori da 0..18060 (dddmm) la
    // conversione in int non e' definita; NAN e la posizione viene scartata.
    if (!(v >= 0.0 && v < 18100.0)) {
        return NAN;
    }
    double deg = (double) ((int) (v / 100));
    double d = deg + (v - deg * 100) / 60.0;
    return negative ? -d : d;
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

bool nmea_checksum_ok(const char *line)
{
    if (!line || line[0] != '$') {
        return false;
    }
    unsigned char cs = 0;
    const char *p = line + 1;
    for (; *p && *p != '*'; p++) {
        cs ^= (unsigned char) *p;
    }
    if (*p != '*') {
        return false;
    }
    int hi = hex_val(p[1]), lo = hi >= 0 ? hex_val(p[2]) : -1;
    return hi >= 0 && lo >= 0 && (unsigned char) ((hi << 4) | lo) == cs;
}

void gnss_fix_parse_gga(const char *line_in)
{
    // Checksum controllato qui (oltre che dal lettore): da questa riga
    // dipende anche la media della misura della posizione base.
    if (!s_mutex || !line_in || line_in[0] != '$' || strlen(line_in) < 6 || !nmea_checksum_ok(line_in)) {
        return;
    }

    char line[128];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    // $xxGGA,hhmmss.ss,lat,N/S,lon,E/W,qualita,numSV,HDOP,alt,M,sep,M,eta_diff,id_stazione*checksum
    char *cursor = line;
    next_field(&cursor); // "$xxGGA"
    next_field(&cursor); // ora
    char *lat = next_field(&cursor);
    char *ns = next_field(&cursor);
    char *lon = next_field(&cursor);
    char *ew = next_field(&cursor);
    char *quality = next_field(&cursor);
    char *num_sv = next_field(&cursor);
    char *hdop = next_field(&cursor);
    char *alt = next_field(&cursor);
    next_field(&cursor); // unita' altitudine (sempre "M")
    char *sep = next_field(&cursor);
    next_field(&cursor); // unita' separazione (sempre "M")
    char *diff_age = next_field(&cursor);

    if (!quality) {
        return; // sentenza troppo corta/malformata
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    s_status.valid = true;
    s_status.quality = (gnss_fix_quality_t) atoi(quality);
    s_status.satellites_used = (num_sv && num_sv[0] != '\0') ? (uint8_t) atoi(num_sv) : 0;
    s_status.hdop = (hdop && hdop[0] != '\0') ? (float) atof(hdop) : -1;
    s_status.altitude_m = (alt && alt[0] != '\0') ? (float) atof(alt) : -9999;
    s_status.diff_age_s = (diff_age && diff_age[0] != '\0') ? (float) atof(diff_age) : -1;
    s_status.geoid_sep_m = (sep && sep[0] != '\0') ? (float) atof(sep) : -9999;
    s_status.has_position = lat && lat[0] != '\0' && lon && lon[0] != '\0' && ns && ew;
    if (s_status.has_position) {
        double la = nmea_coord_deg(lat, ns[0] == 'S');
        double lo = nmea_coord_deg(lon, ew[0] == 'W');
        if (isfinite(la) && isfinite(lo) && fabs(la) <= 90.0 && fabs(lo) <= 180.0) {
            s_status.lat_deg = la;
            s_status.lon_deg = lo;
        } else {
            s_status.has_position = false;
        }
    }
    s_status.last_update_us = esp_timer_get_time();
    xSemaphoreGive(s_mutex);
}

gnss_fix_status_t gnss_fix_get_status(void)
{
    if (!s_mutex) {
        return (gnss_fix_status_t){0};
    }
    gnss_fix_status_t copy;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_mutex);
    return copy;
}

const char *gnss_fix_quality_str(gnss_fix_quality_t q)
{
    switch (q) {
        case GNSS_FIX_NONE: return "Nessun fix";
        case GNSS_FIX_GPS: return "GPS";
        case GNSS_FIX_DGPS: return "DGPS";
        case GNSS_FIX_PPS: return "PPS";
        case GNSS_FIX_RTK_FIXED: return "RTK fisso";
        case GNSS_FIX_RTK_FLOAT: return "RTK float";
        case GNSS_FIX_ESTIMATED: return "Stimato";
        case GNSS_FIX_MANUAL: return "Manuale";
        case GNSS_FIX_SIMULATION: return "Simulazione";
        default: return "Sconosciuto";
    }
}
