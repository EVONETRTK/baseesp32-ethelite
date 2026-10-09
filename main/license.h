#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Attivazione del ricevitore e licenze delle funzioni (docs/attivazione-licenze.md).
//
// La licenza e' un JSON firmato dal server EVONETRTK (ECDSA P-256, SHA-256),
// legato alla matricola e al MAC di fabbrica del chip. Il ricevitore la
// verifica da solo con la chiave pubblica compilata qui sotto, quindi
// funziona anche senza internet; la rinnova quando c'e' rete.
//
// Le funzioni concesse si calcolano UNA volta all'avvio (license_init) e non
// cambiano fino al riavvio: nessuna funzione si spegne mentre si lavora.
//
// LICENSE_ENFORCE 0 = modalita' di prova: il modulo verifica e mostra tutto,
// ma license_has() concede sempre (il server vero non esiste ancora e la
// base di prova deve continuare a lavorare). Da mettere a 1 quando il server
// e' in funzione e i ricevitori esistenti hanno la loro licenza.
#define LICENSE_ENFORCE 0

typedef enum {
    // Pacchetti (decisi l'08/10/2026): il modo di lavoro e' bloccato dal pacchetto.
    LIC_BASE = 0,    // Pacchetto Base: lavorare come base (invio al caster), misura HAS,
                     // caster locale, VPN, monitoraggio, dati grezzi, avvisi, antenna spostata
    LIC_ROVER,       // Pacchetto Rover: lavorare come rover, correzioni dal caster, NMEA 1 Hz
    // Extra del rover, venduti a parte:
    LIC_NMEA_FAST,   // uscita a 5-10 Hz per l'autosterzo
    LIC_HAS,         // riserva HAS del rover
    LIC_INS,         // IMU / doppia antenna (ComNav K922)
    LIC_COUNT
} license_feature_t;

typedef enum {
    LIC_STATE_UNACTIVATED = 0, // nessuna licenza valida
    LIC_STATE_ACTIVE,
    LIC_STATE_EXPIRING,        // qualche funzione extra (che conta: con il Pacchetto Rover) scade entro 30 giorni
    LIC_STATE_EXTRA_EXPIRED,   // funzioni extra (che contano) scadute (si spengono al riavvio)
    LIC_STATE_REVOKED,
} license_state_t;

// All'avvio, dopo NVS e impostazioni e prima di avviare i servizi.
void license_init(void);

// La funzione e' concessa in questo avvio? (con LICENSE_ENFORCE 0 sempre si.)
bool license_has(license_feature_t f);
// Come sopra ma senza la modalita' di prova: per il pannello (lucchetti).
bool license_granted(license_feature_t f);

const char *license_feature_code(license_feature_t f);

typedef struct {
    license_state_t state;
    bool enforce;
    char customer[24];
    char chip[13];
    int64_t issued;
    int64_t renew_after;
    int64_t last_contact;      // ultimo rinnovo riuscito (unix), 0 = mai
    int64_t expiry[LIC_COUNT]; // -1 = non compresa, 0 = per sempre
    bool granted_boot[LIC_COUNT];
    bool busy;                 // attivazione o rinnovo in corso
    char last_msg[256];        // esito dell'ultima operazione
    char server[96];
    bool stored;               // c'e' una licenza salvata (anche non verificabile)
    bool extras_suspended;     // extra sospesi in questo avvio: nessun contatto con il server da oltre 30 giorni
    int no_contact_days;       // giorni dall'ultimo contatto riuscito (o dall'emissione), -1 = non noto
    char note[160];            // spiegazione per il pannello ("" = niente da dire)
} license_status_t;

void license_get_status(license_status_t *out);

// C'e' una licenza salvata? (anche se non piu' verificabile.) Con una
// licenza la matricola non si cambia piu' dal pannello: la licenza e' legata
// alla matricola e non varrebbe piu'.
bool license_is_stored(void);

// Prima scadenza (unix) fra gli extra che contano (licenza con il Pacchetto
// Rover); false se nessun extra ha una scadenza. Per i promemoria (alerts.c).
bool license_next_extra_expiry(int64_t *expiry);

// Attivazione dal pannello (in un task a parte; esito in last_msg).
typedef struct {
    char code[32];
    char who[64];
    bool terms;
    bool clauses_1341;
} license_activation_t;

bool license_request_activate(const license_activation_t *a, char *err, size_t err_size);
bool license_request_renew(char *err, size_t err_size);

// Ogni minuto (dal task degli avvisi): rinnovo quando e' il momento.
void license_tick(void);

// Indirizzo del server (prove: server finto sul PC). Vuoto = predefinito.
// Con LICENSE_ENFORCE 1 solo https://.
bool license_set_server(const char *url, char *err, size_t err_size);

// Assistenza e prove: cancella la licenza salvata (non la chiave del
// ricevitore ne' le impostazioni). Il ricevitore torna "da attivare" e si
// riattiva con il suo codice, come dopo un ripristino di fabbrica.
bool license_forget(char *err, size_t err_size);

// Versione dei testi da accettare (cambia quando cambiano i testi).
#define LICENSE_TERMS_VERSION "2026-10-08-bozza"
