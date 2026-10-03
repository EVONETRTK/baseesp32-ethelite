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

// Manda un poll UBX-MON-VER (nessuna configurazione, solo lettura) e
// ritorna subito - la risposta (versione SW/HW/protocollo) arriva in modo
// asincrono e viene loggata da gnss_ubx_ack.c quando arriva. Diagnostica:
// usata per sapere con certezza cosa risponde davvero il ricevitore, es.
// quando UBX-CFG-VALSET viene rifiutato per motivi non chiari (potrebbe
// essere un ricevitore con protocollo troppo vecchio per VALSET, che
// esiste solo da protocol version 27 in su - vedi PROTVER nella risposta).
esp_err_t gnss_ubx_poll_version(uart_port_t uart_num);

// Riavvio hardware del ricevitore u-blox (UBX-CFG-RST, avvio a caldo: tiene
// effemeridi e almanacco). Lo ZED ha alimentazione propria: il riavvio
// dell'ESP32 non lo riavvia, e se e' lui a essersi bloccato la base
// resterebbe muta anche dopo il riavvio. Va seguito dal riavvio dell'ESP32,
// che riconfigura il ricevitore (la configurazione e' solo in RAM).
void gnss_ubx_hw_reset(void);
