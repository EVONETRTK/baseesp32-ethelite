#pragma once

#include <stdint.h>
#include <stdbool.h>

// Valori del campo qualita' fix della sentenza NMEA $xxGGA (standard, non
// specifico di un chip): 4 e 5 sono gli stati RTK veri e propri.
typedef enum {
    GNSS_FIX_NONE = 0,
    GNSS_FIX_GPS = 1,
    GNSS_FIX_DGPS = 2,
    GNSS_FIX_PPS = 3,
    GNSS_FIX_RTK_FIXED = 4,
    GNSS_FIX_RTK_FLOAT = 5,
    GNSS_FIX_ESTIMATED = 6,
    GNSS_FIX_MANUAL = 7,
    GNSS_FIX_SIMULATION = 8,
} gnss_fix_quality_t;

typedef struct {
    bool valid;                  // true se e' mai arrivata una $GGA
    gnss_fix_quality_t quality;
    uint8_t satellites_used;
    float hdop;                  // -1 se non riportato
    float altitude_m;            // -9999 se non riportato
    float diff_age_s;            // -1 se non riportato (nessuna correzione differenziale)
    int64_t last_update_us;      // esp_timer_get_time() all'ultimo aggiornamento, valido solo se "valid"
} gnss_fix_status_t;

// Inizializza lo stato condiviso del fix. Va chiamata una volta all'avvio
// prima di usare le altre funzioni di questo modulo.
void gnss_fix_init(void);

// Analizza una riga NMEA $xxGGA (senza CR/LF finale) e aggiorna lo stato
// condiviso del fix. Sicura da chiamare da un solo task alla volta (il
// chiamante e' gnss_nmea_reader).
void gnss_fix_parse_gga(const char *line);

// Copia lo stato corrente del fix. Puo' essere chiamata da qualunque task
// (es. il server web).
gnss_fix_status_t gnss_fix_get_status(void);

// Descrizione breve in italiano della qualita' del fix (es. "RTK fisso"),
// utile per log/UI senza duplicare la stessa tabella altrove.
const char *gnss_fix_quality_str(gnss_fix_quality_t q);
