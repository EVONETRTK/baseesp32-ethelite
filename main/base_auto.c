#include "base_auto.h"

#include <math.h>
#include <string.h>

#define EARTH_RADIUS_KM 6371.0088
#define DEG_TO_RAD      (3.14159265358979323846 / 180.0)

double base_auto_distance_km(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg)
{
    double p1 = lat1_deg * DEG_TO_RAD;
    double p2 = lat2_deg * DEG_TO_RAD;
    double dp = (lat2_deg - lat1_deg) * DEG_TO_RAD;
    double dl = (lon2_deg - lon1_deg) * DEG_TO_RAD;
    double sp = sin(dp / 2), sl = sin(dl / 2);
    double a = sp * sp + cos(p1) * cos(p2) * sl * sl;
    // Arrotondamenti: a puo' uscire di poco da [0, 1] (punti agli antipodi).
    if (a < 0) {
        a = 0;
    } else if (a > 1) {
        a = 1;
    }
    return 2 * EARTH_RADIUS_KM * asin(sqrt(a));
}

bool base_auto_coords_valid(double lat_deg, double lon_deg)
{
    if (!isfinite(lat_deg) || !isfinite(lon_deg)) {
        return false;
    }
    if (lat_deg < -90 || lat_deg > 90 || lon_deg < -180 || lon_deg > 180) {
        return false;
    }
    return !(lat_deg == 0 && lon_deg == 0);
}

static void set_name(char *dst, const char *src)
{
    size_t n = strnlen(src, BASE_AUTO_NAME_LEN - 1);
    memcpy(dst, src, n);
    dst[n] = '\0';
}

base_auto_result_t base_auto_choose(const base_auto_entry_t *list, size_t count, bool pos_valid, double lat_deg,
                                    double lon_deg, const char *current, int quality, uint32_t no_rtk_s, bool first)
{
    base_auto_result_t r;
    memset(&r, 0, sizeof(r));
    r.nearest = -1;
    r.current = -1;
    r.nearest_km = -1;
    r.current_km = -1;
    set_name(r.mountpoint, current ? current : "");

    if (!pos_valid || !base_auto_coords_valid(lat_deg, lon_deg)) {
        r.reason = BASE_AUTO_KEEP_NO_POSITION;
        return r;
    }

    for (size_t i = 0; list && i < count; i++) {
        const base_auto_entry_t *e = &list[i];
        if (e->name[0] == '\0' || !base_auto_coords_valid(e->lat_deg, e->lon_deg)) {
            continue; // mountpoint senza coordinate (o con coordinate impossibili)
        }
        r.candidates++;
        double d = base_auto_distance_km(lat_deg, lon_deg, e->lat_deg, e->lon_deg);
        if (r.nearest < 0 || d < r.nearest_km) {
            r.nearest = (int) i;
            r.nearest_km = d;
        }
        if (r.current < 0 && current && strncmp(e->name, current, BASE_AUTO_NAME_LEN) == 0) {
            r.current = (int) i;
            r.current_km = d;
        }
    }
    if (r.candidates == 0) {
        r.reason = BASE_AUTO_KEEP_NO_CANDIDATES;
        return r;
    }
    // A parita' di distanza (stesse coordinate) si tiene quella in uso.
    if (r.current >= 0 && r.current_km <= r.nearest_km) {
        r.nearest = r.current;
        r.nearest_km = r.current_km;
        r.reason = BASE_AUTO_KEEP_NEAREST;
        return r;
    }
    // Mai cambiare in RTK fisso o float: si perderebbe il fix in mezzo al campo.
    if (quality == 4 || quality == 5) {
        r.reason = BASE_AUTO_KEEP_RTK;
        return r;
    }
    if (!first) {
        if (no_rtk_s < BASE_AUTO_NO_RTK_S) {
            r.reason = BASE_AUTO_KEEP_WAIT_NO_RTK;
            return r;
        }
        // Base in uso con coordinate note: si cambia solo per un guadagno
        // vero, non per pochi metri (niente salti avanti e indietro tra due
        // basi quasi alla stessa distanza).
        if (r.current >= 0 && r.current_km - r.nearest_km < BASE_AUTO_HYSTERESIS_KM) {
            r.reason = BASE_AUTO_KEEP_HYSTERESIS;
            return r;
        }
    }
    r.reason = BASE_AUTO_CHANGE;
    r.change = true;
    set_name(r.mountpoint, list[r.nearest].name);
    return r;
}

const char *base_auto_reason_str(base_auto_reason_t r)
{
    switch (r) {
    case BASE_AUTO_KEEP_NO_POSITION:   return "rover senza posizione valida";
    case BASE_AUTO_KEEP_NO_CANDIDATES: return "nessun mountpoint con coordinate nella tabella";
    case BASE_AUTO_KEEP_RTK:           return "rover in RTK, nessun cambio";
    case BASE_AUTO_KEEP_WAIT_NO_RTK:   return "senza RTK da meno di 30 s";
    case BASE_AUTO_KEEP_NEAREST:       return "la base in uso e' gia' la piu' vicina";
    case BASE_AUTO_KEEP_HYSTERESIS:    return "un'altra base e' piu' vicina di meno di 5 km";
    case BASE_AUTO_CHANGE:             return "cambio base";
    default:                           return "?";
    }
}
