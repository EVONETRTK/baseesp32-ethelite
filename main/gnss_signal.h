#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

#define GNSS_SIGNAL_MAX_SATS 32

typedef struct {
    char constellation[3]; // talker id NMEA: "GP","GL","GA","GB", ecc. (senza terminatore garantito nell'array, vedi .c)
    uint16_t prn;
    uint8_t snr;            // dB-Hz, 0 se non riportato dal ricevitore
    bool used;
} gnss_sat_signal_t;

// Inizializza lo stato condiviso satelliti. Va chiamata una volta
// all'avvio prima di usare le altre funzioni di questo modulo.
void gnss_signal_init(void);

// Analizza una riga NMEA $xxGSV (senza CR/LF finale) e aggiorna lo stato
// satelliti condiviso. Sicura da chiamare da un solo task alla volta
// (il chiamante e' gnss_nmea_reader).
void gnss_signal_parse_gsv(const char *line);

// Copia fino a max_sats voci correnti in out; ritorna quante ne ha
// copiate. Puo' essere chiamata da qualunque task (es. il server web).
size_t gnss_signal_get_satellites(gnss_sat_signal_t *out, size_t max_sats);
