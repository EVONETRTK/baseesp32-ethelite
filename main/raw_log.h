#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_http_server.h"

// Registrazione dei dati grezzi del ricevitore u-blox (UBX RXM-RAWX e
// RXM-SFRBX) sulla microSD, per calcolare le coordinate precise della base
// con il PPP (post-elaborazione, es. CSRS-PPP dopo la conversione in RINEX
// con RTKLIB). Un file .ubx per sessione in /sdcard/raw.
//
// I frame arrivano dal flusso del ricevitore (raw_log_feed), passano da un
// buffer nella PSRAM e vengono scritti sulla microSD a blocchi ogni 10 s
// (raw_log_service, dal task del log diagnostico): con il ricevitore via I2C
// la SD montata ferma l'I2C, quindi niente scritture lunghe ne' frequenti.
// L'uscita RAWX/SFRBX del ricevitore la attiva il task del survey-in
// (gnss_ubx.c), l'unico che manda comandi allo ZED.
// Una sessione sopravvive ai riavvii: la fine prevista e' salvata in NVS.

void raw_log_init(void);
void raw_log_feed(const uint8_t *buf, size_t len);   // dal task che legge il ricevitore
void raw_log_service(void);                          // dal task del log diagnostico
bool raw_log_active(void);

// Avvia una sessione di 'hours' ore con le misure ogni 'interval_s' secondi (1, 5 o 30).
bool raw_log_start(uint32_t hours, uint8_t interval_s, char *err, size_t err_size);
void raw_log_stop(const char *why);

// Uscita RAWX richiesta al ricevitore: ogni quante epoche (0 = spenta).
uint8_t raw_log_wanted_rate(void);

typedef struct {
    bool active;
    bool waiting_time;     // attiva ma in attesa dell'ora esatta (NTP) per il nome del file
    char file[64];
    uint64_t bytes;        // scritti sulla SD in questa sessione
    uint32_t frames;       // frame RAWX/SFRBX ricevuti
    uint32_t dropped;      // frame persi (buffer pieno, SD non disponibile a lungo)
    int64_t end_unix;      // fine prevista
    uint8_t interval_s;
    char note[96];         // ultimo esito (fine, errore SD...)
} raw_log_status_t;

void raw_log_get_status(raw_log_status_t *out);

// File registrati (JSON {"sd_ok":..,"files":[{"name":..,"size":..}]}), scaricamento e cancellazione.
esp_err_t raw_log_send_list(httpd_req_t *req);
esp_err_t raw_log_send_file(httpd_req_t *req, const char *name);
bool raw_log_delete(const char *name);
