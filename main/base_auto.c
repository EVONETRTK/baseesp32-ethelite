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

static int find_ban(const base_auto_ban_t bans[BASE_AUTO_BANS], const char *name)
{
    for (int i = 0; i < BASE_AUTO_BANS; i++) {
        if (bans[i].until_us != 0 && strncmp(bans[i].name, name, BASE_AUTO_NAME_LEN) == 0) {
            return i;
        }
    }
    return -1;
}

int base_auto_ban_add(base_auto_ban_t bans[BASE_AUTO_BANS], const char *name, int64_t now_us)
{
    if (!bans || !name || !name[0]) {
        return 0;
    }
    // strikes = esclusioni gia' fatte di fila; la durata e' 1 h << strikes.
    // Una base ancora esclusa (rinnovo: nessuna alternativa, o rifiuti di
    // fila senza altre basi) non e' stata riprovata dopo la scadenza: stessa
    // durata e stesso conto, altrimenti arrivava a 4 h in pochi minuti
    // (revisione della 1.35.24). Si sale solo se l'esclusione era scaduta.
    int strikes = 0;
    bool renew = false;
    int slot = find_ban(bans, name);
    if (slot >= 0) {
        if (bans[slot].until_us > now_us) {
            renew = true;
            strikes = bans[slot].strikes > 0 ? bans[slot].strikes - 1 : 0;
        } else if (bans[slot].until_us > now_us - (int64_t) BASE_AUTO_BAN_MAX_S * 1000000) {
            strikes = bans[slot].strikes; // scaduta da poco e di nuovo muta: si sale
        }
    } else {
        slot = 0;
        for (int i = 1; i < BASE_AUTO_BANS; i++) {
            if (bans[i].until_us < bans[slot].until_us) {
                slot = i; // libero o scaduto (o quello che scade prima)
            }
        }
    }
    if (strikes < 0) {
        strikes = 0;
    }
    int64_t s = BASE_AUTO_BAN_S;
    for (int k = 0; k < strikes && s < BASE_AUTO_BAN_MAX_S; k++) {
        s *= 2;
    }
    if (s > BASE_AUTO_BAN_MAX_S) {
        s = BASE_AUTO_BAN_MAX_S;
    }
    set_name(bans[slot].name, name);
    bans[slot].until_us = now_us + s * 1000000;
    // rinnovo: conto invariato (strikes era stato ridotto di uno sopra)
    bans[slot].strikes = (renew || strikes < 100) ? strikes + 1 : strikes;
    return (int) s;
}

void base_auto_ban_forgive(base_auto_ban_t bans[BASE_AUTO_BANS], const char *name)
{
    if (!bans || !name) {
        return;
    }
    int i = find_ban(bans, name);
    if (i >= 0) {
        bans[i].until_us = 0; // posto libero: niente esclusione e conto a zero
        bans[i].strikes = 0;
        bans[i].name[0] = '\0';
    }
}

int base_auto_ban_left_s(const base_auto_ban_t bans[BASE_AUTO_BANS], const char *name, int64_t now_us)
{
    if (!bans || !name) {
        return 0;
    }
    int i = find_ban(bans, name);
    if (i < 0 || bans[i].until_us <= now_us) {
        return 0;
    }
    return (int) ((bans[i].until_us - now_us + 999999) / 1000000);
}

base_auto_silent_t base_auto_silent_session(base_auto_ban_t bans[BASE_AUTO_BANS], base_auto_silent_ctr_t *ctr,
                                            bool got_data, const char *current, const char *fallback, int64_t now_us)
{
    if (strncmp(ctr->name, current, BASE_AUTO_NAME_LEN) != 0) {
        set_name(ctr->name, current); // altra base: il conto riparte
        ctr->count = 0;
    }
    if (got_data) {
        ctr->count = 0;
        base_auto_ban_forgive(bans, current);
        return BASE_AUTO_SILENT_KEEP;
    }
    if (++ctr->count < BASE_AUTO_SILENT_SESSIONS) {
        return BASE_AUTO_SILENT_KEEP;
    }
    ctr->count = 0;
    bool was_banned = base_auto_banned(bans, current, now_us);
    base_auto_ban_add(bans, current, now_us);
    if (was_banned) {
        return BASE_AUTO_SILENT_RENEWED;
    }
    if (strncmp(current, fallback, BASE_AUTO_NAME_LEN) != 0 && !base_auto_banned(bans, fallback, now_us)) {
        return BASE_AUTO_SILENT_FALLBACK;
    }
    return BASE_AUTO_SILENT_RECHOOSE;
}

bool base_auto_banned(const base_auto_ban_t bans[BASE_AUTO_BANS], const char *name, int64_t now_us)
{
    if (!bans || !name) {
        return false;
    }
    for (int i = 0; i < BASE_AUTO_BANS; i++) {
        if (bans[i].until_us > now_us && strncmp(bans[i].name, name, BASE_AUTO_NAME_LEN) == 0) {
            return true;
        }
    }
    return false;
}

base_auto_result_t base_auto_choose(const base_auto_entry_t *list, size_t count, bool pos_valid, double lat_deg,
                                    double lon_deg, const char *current, int quality, uint32_t no_rtk_s, bool first)
{
    return base_auto_choose_ex(list, count, pos_valid, lat_deg, lon_deg, current, quality, no_rtk_s, first, NULL, 0);
}

base_auto_result_t base_auto_choose_ex(const base_auto_entry_t *list, size_t count, bool pos_valid, double lat_deg,
                                       double lon_deg, const char *current, int quality, uint32_t no_rtk_s, bool first,
                                       const base_auto_ban_t *bans, int64_t now_us)
{
    // Base in uso esclusa: va lasciata come al primo collegamento.
    if (current && base_auto_banned(bans, current, now_us)) {
        first = true;
    }
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
        if (base_auto_banned(bans, e->name, now_us)) {
            continue; // muta da poco
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
