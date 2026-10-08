#pragma once

#include "driver/uart.h"
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

// Configura un ricevitore Unicore (UM980/UM982) collegato sulla UART
// indicata come base RTK: posizione fissa o survey-in, uscita RTCM3 e NMEA
// (GGA/GSV per lo stato nel pannello) sulla stessa porta.
//
// ATTENZIONE: comandi presi dai file di configurazione UM980/UM982 della
// base Raspberry (ELT_RTKBase, usati sul campo) e dalla documentazione
// Unicore UM98x, ma non ancora provati su un modulo collegato a questa
// scheda: controllare il log all'avvio ("gnss_unicore") la prima volta.
esp_err_t gnss_unicore_configure_base(uart_port_t uart_num);

// Configura lo stesso ricevitore come rover: esce dalla modalita' base e
// abilita l'uscita NMEA GGA (usata da ntrip_rover_client.c per inoltrare
// la posizione al caster). Stesse avvertenze.
esp_err_t gnss_unicore_configure_rover(uart_port_t uart_num);

// Righe ASCII Unicore con '#' dal ricevitore (base_stream_demux.c in base,
// gnss_nmea_reader.c in rover): #VERSIONA (modello e firmware) e #PPPNAVA
// (soluzione PPP / Galileo HAS).
void gnss_unicore_note_line(const char *line);

// Ultima soluzione PPP (log PPPNAVA, 1 al secondo con HAS attivo). Con
// l'UM982 la soluzione HAS esce SOLO qui: la GGA resta la soluzione
// autonoma (verificato il 07/10/2026 sulla base ALTAMURA, firmware 13495).
typedef struct {
    bool valid;            // SOL_COMPUTED con un tipo PPP_*
    char type[24];         // es. "PPP_CONVERGING"
    double lat_deg, lon_deg;
    double h_ell_m;        // quota ellissoidica (altezza + ondulazione)
    float sig_lat_m, sig_lon_m, sig_h_m; // incertezza dichiarata
    float corr_age_s;      // eta' delle correzioni HAS
    int sats_used;
    int64_t at_us;         // esp_timer_get_time() della riga (0 = mai)
} gnss_unicore_ppp_t;

void gnss_unicore_ppp_get(gnss_unicore_ppp_t *out);

// Modello ("UM982", "UM980"...) e firmware; stringa vuota se non letti.
const char *gnss_unicore_model(void);
const char *gnss_unicore_fw(void);
