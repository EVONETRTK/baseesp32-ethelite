#pragma once

#include "driver/uart.h"
#include "esp_err.h"

// Configura un ricevitore u-blox (serie F9, es. ZED-F9P) collegato sulla
// UART indicata come base RTK: Survey-In automatico e uscita RTCM3 sulla
// stessa porta seriale.
//
// ATTENZIONE: le chiavi di configurazione UBX-CFG-VALSET usate qui (vedi
// gnss_ubx.c) sono quelle documentate pubblicamente per la famiglia
// u-blox F9 ma sono state trascritte a memoria, senza possibilita' di
// verifica su hardware reale in questo progetto (ESP-IDF non installato).
// La funzione non legge/verifica gli UBX-ACK di risposta: un ID chiave
// sbagliato viene tipicamente rifiutato in silenzio dal ricevitore
// (nessun crash, ma nessuna configurazione applicata). Vanno controllate
// contro l'Interface Description del modulo specifico prima dell'uso in
// campo.
esp_err_t gnss_ubx_configure_base(uart_port_t uart_num);

// Configura lo stesso ricevitore come rover: esce dalla modalita' base,
// accetta RTCM3 in ingresso sulla stessa UART e riabilita l'uscita NMEA
// (usata da ntrip_rover_client.c per inoltrare $GxGGA al caster). Stesse
// avvertenze di gnss_ubx_configure_base sulle chiavi UBX non verificate.
esp_err_t gnss_ubx_configure_rover(uart_port_t uart_num);
