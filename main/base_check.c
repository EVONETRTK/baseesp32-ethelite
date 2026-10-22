#include "base_check.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

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

// Numero di bit a 1 (costellazioni presenti in una maschera BASE_CHECK_*).
static int count_bits(uint8_t v)
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
    r.n_systems = count_bits(r.systems);

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

// Costellazioni scritte nel campo 7 della tabella delle sorgenti.
static uint8_t navsys_bits(const char *s)
{
    static const struct {
        const char *name;
        uint8_t bit;
    } map[] = {
        { "GPS", BASE_CHECK_GPS },         { "GLO", BASE_CHECK_GLONASS }, { "GAL", BASE_CHECK_GALILEO },
        { "BDS", BASE_CHECK_BEIDOU },      { "BEI", BASE_CHECK_BEIDOU },  { "COM", BASE_CHECK_BEIDOU },
        { "QZS", BASE_CHECK_QZSS },        { "SBA", BASE_CHECK_SBAS },    { "IRN", BASE_CHECK_NAVIC },
        { "NAV", BASE_CHECK_NAVIC },
    };
    uint8_t bits = 0;
    while (s && *s) {
        while (*s && !isalpha((unsigned char) *s)) {
            s++;
        }
        char tok[4] = { 0 };
        size_t n = 0;
        while (*s && isalpha((unsigned char) *s)) {
            if (n < 3) {
                tok[n++] = (char) toupper((unsigned char) *s);
            }
            s++;
        }
        for (size_t i = 0; n == 3 && i < sizeof(map) / sizeof(map[0]); i++) {
            if (strcmp(tok, map[i].name) == 0) {
                bits |= map[i].bit;
            }
        }
    }
    return bits;
}

base_check_t base_check_from_sourcetable(const char *details, const char *navsys, double distance_km,
                                         bool *types_known)
{
    uint16_t types[64];
    size_t nt = 0;
    const char *p = details ? details : "";
    while (*p && nt < sizeof(types) / sizeof(types[0])) {
        if (isdigit((unsigned char) *p)) {
            char *end = NULL;
            long v = strtol(p, &end, 10);
            // Solo i numeri di messaggio (1001-4095), non le frequenze tra
            // parentesi ("1077(1)").
            if (v >= 1001 && v <= 4095 && (p == details || p[-1] != '(')) {
                types[nt++] = (uint16_t) v;
            }
            p = end;
        } else {
            p++;
        }
    }
    bool known = false;
    for (size_t i = 0; i < nt; i++) {
        uint16_t t = types[i];
        if ((t >= 1001 && t <= 1012) || (t >= 1071 && t <= 1137)) {
            known = true; // c'e' almeno un messaggio di osservazione
        }
    }
    if (types_known) {
        *types_known = known;
    }
    base_check_t r;
    if (known) {
        r = base_check_eval(types, nt, distance_km);
    } else {
        // Tipi sconosciuti: niente giudizio su messaggi e posizione.
        r = base_check_eval(NULL, 0, distance_km);
        r.issues = r.issues & (BASE_CHECK_FAR | BASE_CHECK_TOO_FAR);
        r.level = (r.issues & BASE_CHECK_TOO_FAR) ? BASE_CHECK_LEVEL_BAD
                  : (r.issues & BASE_CHECK_FAR)   ? BASE_CHECK_LEVEL_WARN
                                                  : BASE_CHECK_LEVEL_OK;
    }
    // Le costellazioni dichiarate nel campo 7 si aggiungono a quelle dei tipi.
    r.systems |= navsys_bits(navsys);
    r.n_systems = count_bits(r.systems);
    r.issues &= ~(uint32_t) BASE_CHECK_NO_POSITION; // spesso omessa dalle tabelle
    r.has_position = true;
    if (r.n_systems >= 3) {
        r.issues &= ~(uint32_t) BASE_CHECK_FEW_SYSTEMS;
    } else if (r.n_systems > 0) {
        r.issues |= BASE_CHECK_FEW_SYSTEMS;
    }
    if (!(r.systems & BASE_CHECK_GLONASS)) {
        r.issues &= ~(uint32_t) BASE_CHECK_GLO_NO_1230;
    }
    if (!known && r.n_systems == 0) {
        r.issues &= ~(uint32_t) BASE_CHECK_NO_DATA;
    }
    // Livello ricalcolato sui problemi rimasti.
    if (r.issues & BASE_CHECK_TOO_FAR) {
        r.level = BASE_CHECK_LEVEL_BAD;
    } else if (r.issues & (BASE_CHECK_LEGACY_ONLY | BASE_CHECK_FEW_SYSTEMS | BASE_CHECK_FAR)) {
        r.level = BASE_CHECK_LEVEL_WARN;
    } else {
        r.level = BASE_CHECK_LEVEL_OK;
    }
    return r;
}

const char *base_check_level_str(base_check_level_t l)
{
    switch (l) {
    case BASE_CHECK_LEVEL_OK: return "ok";
    case BASE_CHECK_LEVEL_WARN: return "attenzione";
    default: return "problema";
    }
}
