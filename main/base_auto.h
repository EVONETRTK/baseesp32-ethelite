#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

// Scelta automatica della base piu' vicina per il rover (impostazione
// rover_auto_base): dalla tabella delle sorgenti del caster (righe STR con
// latitudine e longitudine) si sceglie il mountpoint piu' vicino alla
// posizione del rover. Per non perdere il fix in mezzo al campo la base si
// cambia solo quando il rover NON e' in RTK (fisso o float) da almeno
// BASE_AUTO_NO_RTK_S secondi, oppure al primo collegamento, e solo se
// un'altra base e' piu' vicina di almeno BASE_AUTO_HYSTERESIS_KM di quella
// in uso.
//
// Solo calcolo, senza ESP-IDF ne' FreeRTOS (prove sul PC in
// tests/host/test_base_auto.c): chi legge la tabella e gestisce il
// collegamento e' ntrip_rover_client.c.

#define BASE_AUTO_HYSTERESIS_KM 5.0
#define BASE_AUTO_NO_RTK_S      30
#define BASE_AUTO_CHECK_S       600  // tabella riletta al massimo ogni 10 minuti
#define BASE_AUTO_NAME_LEN      33   // come rover_mountpoint

typedef struct {
    char name[BASE_AUTO_NAME_LEN];
    double lat_deg;
    double lon_deg;
} base_auto_entry_t;

typedef enum {
    BASE_AUTO_KEEP_NO_POSITION,   // rover senza posizione valida: resta la base attuale
    BASE_AUTO_KEEP_NO_CANDIDATES, // nessun mountpoint con coordinate valide
    BASE_AUTO_KEEP_RTK,           // rover in RTK fisso o float: mai cambiare
    BASE_AUTO_KEEP_WAIT_NO_RTK,   // senza RTK da meno di BASE_AUTO_NO_RTK_S
    BASE_AUTO_KEEP_NEAREST,       // la base in uso e' gia' la piu' vicina
    BASE_AUTO_KEEP_HYSTERESIS,    // un'altra e' piu' vicina, ma di meno di 5 km
    BASE_AUTO_CHANGE,             // cambiare base (mountpoint in out.mountpoint)
} base_auto_reason_t;

typedef struct {
    base_auto_reason_t reason;
    bool change;                         // true solo con BASE_AUTO_CHANGE
    char mountpoint[BASE_AUTO_NAME_LEN]; // base da usare (la piu' vicina se change, altrimenti quella attuale)
    int nearest;                         // indice della base piu' vicina nell'elenco (-1 se nessuna)
    double nearest_km;                   // distanza dalla base piu' vicina (-1 se nessuna)
    int current;                         // indice della base in uso nell'elenco (-1 se assente o senza coordinate)
    double current_km;                   // distanza dalla base in uso (-1 se sconosciuta)
    int candidates;                      // mountpoint con coordinate valide
} base_auto_result_t;

// Distanza in km tra due punti (formula dell'haversine, sfera di raggio
// 6371,0088 km, raggio medio dell'ellissoide WGS84): per scegliere la base
// l'errore rispetto all'ellissoide (fino a ~0,5%) non conta.
double base_auto_distance_km(double lat1_deg, double lon1_deg, double lat2_deg, double lon2_deg);

// true se lat/lon sono numeri finiti, dentro i limiti (+-90, +-180) e non
// entrambi 0 (caster che lasciano le coordinate a zero).
bool base_auto_coords_valid(double lat_deg, double lon_deg);

// Decide quale base usare.
//   list/count       mountpoint della tabella delle sorgenti (anche senza coordinate: si saltano)
//   pos_valid, lat/lon  ultima posizione valida del rover
//   current          mountpoint in uso (puo' non essere nell'elenco)
//   quality          qualita' della GGA (4 fisso, 5 float; -1 nessun dato)
//   no_rtk_s         secondi da cui il rover non e' in RTK (fisso o float)
//   first            primo collegamento (all'avvio o appena attivata la
//                    scelta automatica): si sceglie subito la piu' vicina,
//                    senza attendere i 30 s e senza isteresi (ma mai in RTK)
base_auto_result_t base_auto_choose(const base_auto_entry_t *list, size_t count, bool pos_valid, double lat_deg,
                                    double lon_deg, const char *current, int quality, uint32_t no_rtk_s, bool first);

// Basi escluse per un po' dalla scelta: elencate nella tabella del caster ma
// mute (collegamento accettato e nessun dato, base spenta o guasta). Senza,
// il rover restava per sempre sulla base piu' vicina senza correzioni anche
// con la base iniziale funzionante (collaudo del 22/10/2026).
#define BASE_AUTO_SILENT_SESSIONS 2     // collegamenti di fila senza un byte
#define BASE_AUTO_BAN_S           3600  // esclusione di un'ora, poi si riprova
#define BASE_AUTO_BANS            4

typedef struct {
    char name[BASE_AUTO_NAME_LEN];
    int64_t until_us; // 0 = posto libero
} base_auto_ban_t;

// Esclude name fino a now_us + BASE_AUTO_BAN_S (rinnova se gia' escluso;
// a elenco pieno sostituisce l'esclusione che scade prima).
void base_auto_ban_add(base_auto_ban_t bans[BASE_AUTO_BANS], const char *name, int64_t now_us);
bool base_auto_banned(const base_auto_ban_t bans[BASE_AUTO_BANS], const char *name, int64_t now_us);

// Fine di un collegamento con la scelta automatica attiva: conta i
// collegamenti di fila senza un byte (*silent) e, al BASE_AUTO_SILENT_SESSIONS,
// esclude la base in uso, anche se e' la base iniziale (fallback). Prima la
// base iniziale non si escludeva mai: se era lei la muta il rover ci tornava
// per sempre (collaudo del 23/10/2026, AGRI_MSM4 muta a 0,4 km).
typedef enum {
    BASE_AUTO_SILENT_KEEP,     // dati arrivati, o non ancora abbastanza collegamenti muti
    BASE_AUTO_SILENT_FALLBACK, // esclusa: tornare alla base iniziale
    BASE_AUTO_SILENT_RECHOOSE, // esclusa, e la base iniziale e' lei o e' esclusa: rifare la scelta tra le altre
} base_auto_silent_t;

base_auto_silent_t base_auto_silent_session(base_auto_ban_t bans[BASE_AUTO_BANS], int *silent, bool got_data,
                                            const char *current, const char *fallback, int64_t now_us);

// Come base_auto_choose, ma le basi escluse (bans, puo' essere NULL) non
// sono candidate; se e' esclusa quella in uso si sceglie la piu' vicina
// delle altre senza attese ne' isteresi (mai in RTK).
base_auto_result_t base_auto_choose_ex(const base_auto_entry_t *list, size_t count, bool pos_valid, double lat_deg,
                                       double lon_deg, const char *current, int quality, uint32_t no_rtk_s, bool first,
                                       const base_auto_ban_t *bans, int64_t now_us);

// Descrizione breve del motivo, per il log.
const char *base_auto_reason_str(base_auto_reason_t r);
