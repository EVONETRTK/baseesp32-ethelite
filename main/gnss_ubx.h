#pragma once

#include "driver/uart.h"
#include "esp_err.h"

// Configura un ricevitore u-blox (serie F9, es. ZED-F9P) collegato sulla
// UART indicata come base RTK: Survey-In automatico e uscita RTCM3 (i
// messaggi/costellazioni scelti in settings.rtcm_*) sulla stessa porta
// seriale.
//
// Le chiavi di configurazione UBX-CFG-VALSET usate qui (vedi gnss_ubx.c)
// sono state verificate contro sparkfun/SparkFun_u-blox_GNSS_Arduino_Library
// (src/u-blox_config_keys.h) dopo aver scoperto che i valori usati in
// precedenza in questo file (trascritti a memoria) erano sbagliati - quasi
// tutte le chiavi CFG-MSGOUT-RTCM_3X_TYPE*_UART1 erano spostate di una
// posizione, e le due chiavi CFG-TMODE-SVIN-MIN-DUR/SVIN-ACC-LIMIT erano
// scambiate tra loro. Restano comunque una fonte di terzi, non
// l'Interface Description ufficiale u-blox diretta, e la funzione non
// legge/verifica gli UBX-ACK di risposta: un ID chiave sbagliato viene
// tipicamente rifiutato in silenzio dal ricevitore (nessun crash, ma
// nessuna configurazione applicata) - non ancora testato su un vero
// ricevitore u-blox in questo progetto.
esp_err_t gnss_ubx_configure_base(uart_port_t uart_num);

// Configura lo stesso ricevitore come rover: esce dalla modalita' base,
// accetta RTCM3 in ingresso sulla stessa UART e riabilita l'uscita NMEA
// (usata da ntrip_rover_client.c per inoltrare $GxGGA al caster). Stesse
// avvertenze di gnss_ubx_configure_base sulle chiavi UBX non verificate.
esp_err_t gnss_ubx_configure_rover(uart_port_t uart_num);
