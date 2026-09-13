#pragma once

#include <stdbool.h>
#include <stddef.h>

// Inizializza il modem SIM7600 (UART + netif PPP tramite esp_modem). Va
// chiamata dopo esp_netif_init()/esp_event_loop_create_default(). Ritorna
// false se il supporto cellulare e' disabilitato in Kconfig o se
// l'inizializzazione fallisce.
bool cellular_link_init(void);

// Avvia la connessione dati (PPP) e blocca fino all'ottenimento di un IP
// o a un timeout interno. Ritorna false se il supporto e' disabilitato,
// non inizializzato, o la connessione fallisce.
bool cellular_link_connect(void);

// Riporta il modem in modalita' comando (chiude la sessione dati PPP).
void cellular_link_disconnect(void);

bool cellular_link_is_connected(void);

// Qualita' del segnale (comando AT+CSQ). rssi_dbm e' gia' convertito in
// dBm dalla scala CSQ 0-31 (formula standard: -113 + 2*csq). Ritorna
// false se il supporto cellulare e' disabilitato, il modem non e'
// inizializzato, o la lettura fallisce (es. nessuna copertura).
bool cellular_link_get_signal(int *rssi_dbm);

// Nome operatore e tecnologia di accesso correnti (comando AT+COPS?). La
// tecnologia e' derivata dal codice standard 3GPP "AcT" e mappata su
// 2G/3G/4G/5G in modo generico. Ritorna false se il supporto e'
// disabilitato, il modem non e' inizializzato, o la lettura fallisce
// (es. non ancora registrato in rete).
bool cellular_link_get_operator_info(char *operator_out, size_t operator_out_size,
                                      char *tech_out, size_t tech_out_size);
