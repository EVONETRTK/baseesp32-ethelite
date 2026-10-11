#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Verifica della base di correzione del rover: dai tipi di messaggio RTCM3
// arrivati di recente e dalla distanza dalla base dice se con quella base
// si puo' arrivare all'RTK fisso, e se no perche'. Nata il 10/10/2026: il
// rover restava in float con una base a centinaia di km ("centipede") e
// poi con una che mandava solo messaggi vecchi 1004/1012 ("puglia"), e
// dal pannello non si capiva.
//
// Solo calcolo, senza ESP-IDF ne' FreeRTOS (prove sul PC in
// tests/host/test_base_check.c): chi raccoglie i tipi e la distanza e'
// ntrip_rover_client.c.

#define BASE_CHECK_FAR_KM     35.0 // oltre: fisso lento o instabile
#define BASE_CHECK_TOO_FAR_KM 60.0 // oltre: fisso quasi impossibile

// Costellazioni (bit di base_check_t.systems / msm_systems).
#define BASE_CHECK_GPS     (1u << 0)
#define BASE_CHECK_GLONASS (1u << 1)
#define BASE_CHECK_GALILEO (1u << 2)
#define BASE_CHECK_BEIDOU  (1u << 3)
#define BASE_CHECK_QZSS    (1u << 4)
#define BASE_CHECK_SBAS    (1u << 5)
#define BASE_CHECK_NAVIC   (1u << 6)
#define BASE_CHECK_SYSTEMS 7

// Problemi trovati (bit di base_check_t.issues).
#define BASE_CHECK_NO_DATA      (1u << 0) // nessun messaggio dalla base
#define BASE_CHECK_NO_POSITION  (1u << 1) // manca 1005/1006: il ricevitore non sa dov'e' la base
#define BASE_CHECK_LEGACY_ONLY  (1u << 2) // solo messaggi vecchi (1001-1004, 1009-1012), niente MSM
#define BASE_CHECK_FEW_SYSTEMS  (1u << 3) // meno di 3 costellazioni
#define BASE_CHECK_GLO_NO_1230  (1u << 4) // GLONASS senza 1230: GLONASS poco utile al fisso
#define BASE_CHECK_FAR          (1u << 5) // oltre BASE_CHECK_FAR_KM
#define BASE_CHECK_TOO_FAR      (1u << 6) // oltre BASE_CHECK_TOO_FAR_KM

typedef enum {
    BASE_CHECK_LEVEL_OK = 0,
    BASE_CHECK_LEVEL_WARN = 1, // si lavora, ma il fisso arriva tardi o non tiene
    BASE_CHECK_LEVEL_BAD = 2,  // fisso molto difficile o impossibile
} base_check_level_t;

typedef struct {
    base_check_level_t level;
    uint32_t issues;      // BASE_CHECK_NO_DATA ...
    uint8_t systems;      // costellazioni con osservazioni (MSM o vecchie)
    uint8_t msm_systems;  // costellazioni con messaggi MSM
    int n_systems;        // quante costellazioni in systems
    bool has_position;    // 1005 o 1006 presenti
    bool has_glo_bias;    // 1230 presente
    double distance_km;   // come ricevuta (-1 = sconosciuta)
} base_check_t;

// types/count: tipi RTCM3 arrivati di recente (anche ripetuti, in qualunque
// ordine). distance_km: distanza rover-base, negativa se sconosciuta (in
// quel caso nessun giudizio sulla distanza).
base_check_t base_check_eval(const uint16_t *types, size_t count, double distance_km);

// Nome breve della costellazione di un bit BASE_CHECK_GPS... ("GPS",
// "GLONASS"...), NULL se il bit non e' uno di quelli.
const char *base_check_system_name(uint8_t bit);

// Codice breve di un problema (bit BASE_CHECK_NO_DATA...), per il pannello:
// "nessun_dato", "senza_posizione", "solo_vecchi", "poche_costellazioni",
// "glonass_senza_1230", "lontana", "troppo_lontana". NULL se sconosciuto.
const char *base_check_issue_code(uint32_t bit);

// "ok", "attenzione", "problema".
const char *base_check_level_str(base_check_level_t l);

// Stessa verifica sui dati della tabella delle sorgenti del caster, per
// proporre i mountpoint prima di collegarsi:
//   details  campo 5 della riga STR, i tipi di messaggio ("1004,1005(10),...";
//            spesso vuoto o parziale)
//   navsys   campo 7, le costellazioni ("GPS+GLO+GAL+BDS")
// Con i tipi sconosciuti si giudicano solo costellazioni e distanza; la
// mancanza della 1005/1006 non si segnala (le tabelle spesso la omettono).
// types_known dice se details conteneva almeno un tipo.
base_check_t base_check_from_sourcetable(const char *details, const char *navsys, double distance_km,
                                         bool *types_known);
