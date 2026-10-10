#include "base_check.h"

#include <math.h>

// Costellazione dei messaggi di osservazione: MSM (10x1-10x7) e vecchi
// (1001-1004 GPS, 1009-1012 GLONASS). 0 se il tipo non e' un'osservazione.
static uint8_t msm_system(uint16_t t)
{
    if (t < 1071 || t > 1137 || t % 10 < 1 || t % 10 > 7) {
        return 0;
    }
    switch (t / 10) {
    case 107: return BASE_CHECK_GPS;
    case 108: return BASE_CHECK_GLONASS;
    case 109: return BASE_CHECK_GALILEO;
    case 110: return BASE_CHECK_SBAS;
    case 111: return BASE_CHECK_QZSS;
    case 112: return BASE_CHECK_BEIDOU;
    case 113: return BASE_CHECK_NAVIC;
    default: return 0;
    }
}

static uint8_t legacy_system(uint16_t t)
{
    if (t >= 1001 && t <= 1004) {
        return BASE_CHECK_GPS;
    }
    if (t >= 1009 && t <= 1012) {
        return BASE_CHECK_GLONASS;
    }
    return 0;
}

static int bits(uint8_t v)
{
    int n = 0;
    for (; v; v &= (uint8_t) (v - 1)) {
        n++;
    }
    return n;
}

base_check_t base_check_eval(const uint16_t *types, size_t count, double distance_km)
{
    base_check_t r = { 0 };
    r.distance_km = (isfinite(distance_km) && distance_km >= 0) ? distance_km : -1;
    uint8_t legacy = 0;
    for (size_t i = 0; types && i < count; i++) {
        uint16_t t = types[i];
        r.msm_systems |= msm_system(t);
        legacy |= legacy_system(t);
        if (t == 1005 || t == 1006) {
            r.has_position = true;
        }
        if (t == 1230) {
            r.has_glo_bias = true;
        }
    }
    r.systems = (uint8_t) (r.msm_systems | legacy);
    r.n_systems = bits(r.systems);

    if (count == 0 || !types) {
        r.issues |= BASE_CHECK_NO_DATA;
        r.level = BASE_CHECK_LEVEL_BAD;
        return r;
    }
    if (r.systems == 0 || !r.has_position) {
        // Senza osservazioni o senza la posizione della base il ricevitore
        // non puo' calcolare l'RTK.
        if (!r.has_position) {
            r.issues |= BASE_CHECK_NO_POSITION;
        }
        if (r.systems == 0) {
            r.issues |= BASE_CHECK_NO_DATA;
        }
        r.level = BASE_CHECK_LEVEL_BAD;
    }
    if (r.systems != 0 && r.msm_systems == 0) {
        r.issues |= BASE_CHECK_LEGACY_ONLY;
    }
    if (r.systems != 0 && r.n_systems < 3) {
        r.issues |= BASE_CHECK_FEW_SYSTEMS;
    }
    if ((r.systems & BASE_CHECK_GLONASS) && !r.has_glo_bias) {
        r.issues |= BASE_CHECK_GLO_NO_1230; // solo informativo: non cambia il livello
    }
    if (r.distance_km > BASE_CHECK_TOO_FAR_KM) {
        r.issues |= BASE_CHECK_TOO_FAR;
        r.level = BASE_CHECK_LEVEL_BAD;
    } else if (r.distance_km > BASE_CHECK_FAR_KM) {
        r.issues |= BASE_CHECK_FAR;
    }
    if (r.level == BASE_CHECK_LEVEL_OK &&
        (r.issues & (BASE_CHECK_LEGACY_ONLY | BASE_CHECK_FEW_SYSTEMS | BASE_CHECK_FAR))) {
        r.level = BASE_CHECK_LEVEL_WARN;
    }
    return r;
}

const char *base_check_system_name(uint8_t bit)
{
    switch (bit) {
    case BASE_CHECK_GPS: return "GPS";
    case BASE_CHECK_GLONASS: return "GLONASS";
    case BASE_CHECK_GALILEO: return "Galileo";
    case BASE_CHECK_BEIDOU: return "BeiDou";
    case BASE_CHECK_QZSS: return "QZSS";
    case BASE_CHECK_SBAS: return "SBAS";
    case BASE_CHECK_NAVIC: return "NavIC";
    default: return NULL;
    }
}

const char *base_check_issue_code(uint32_t bit)
{
    switch (bit) {
    case BASE_CHECK_NO_DATA: return "nessun_dato";
    case BASE_CHECK_NO_POSITION: return "senza_posizione";
    case BASE_CHECK_LEGACY_ONLY: return "solo_vecchi";
    case BASE_CHECK_FEW_SYSTEMS: return "poche_costellazioni";
    case BASE_CHECK_GLO_NO_1230: return "glonass_senza_1230";
    case BASE_CHECK_FAR: return "lontana";
    case BASE_CHECK_TOO_FAR: return "troppo_lontana";
    default: return NULL;
    }
}

const char *base_check_level_str(base_check_level_t l)
{
    switch (l) {
    case BASE_CHECK_LEVEL_OK: return "ok";
    case BASE_CHECK_LEVEL_WARN: return "attenzione";
    default: return "problema";
    }
}
