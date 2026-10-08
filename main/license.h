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
    LIC_RTK = 0,     // base: invio al caster; rover: correzioni, NMEA 1 Hz
    LIC_NMEA_FAST,   // uscita a 5-10 Hz per l'autosterzo
    LIC_HAS,         // misura HAS della base, riserva HAS del rover
    LIC_INS,         // IMU / doppia antenna (ComNav K922)
    LIC_BASE_PRO,    // caster locale, VPN, monitoraggio remoto, dati grezzi, avvisi
    LIC_COUNT
} license_feature_t;

typedef enum {
    LIC_STATE_UNACTIVATED = 0, // nessuna licenza valida
    LIC_STATE_ACTIVE,
    LIC_STATE_EXPIRING,        // qualche funzione extra scade entro 30 giorni
    LIC_STATE_EXTRA_EXPIRED,   // funzioni extra scadute (si spengono al riavvio)
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
    char last_msg[128];        // esito dell'ultima operazione
    char server[96];
} license_status_t;

void license_get_status(license_status_t *out);

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
bool license_set_server(const char *url, char *err, size_t err_size);

// Versione dei testi da accettare (cambia quando cambiano i testi).
#define LICENSE_TERMS_VERSION "2026-10-08-bozza"
