#pragma once

#include "driver/uart.h"
#include "esp_err.h"

// Configura un ricevitore Bynav (M20/M20D) collegato sulla UART indicata
// come base RTK: survey-in automatico (FIX AUTO) e uscita RTCM3 sulla
// stessa porta.
//
// ATTENZIONE: nessun hardware Bynav disponibile in questo progetto al
// momento della scrittura - i comandi ASCII usati qui (vedi gnss_bynav.c)
// vengono dal manuale ufficiale "AN085_Set-up and Configuration for Base
// and Rover" (scaricato da bynav.com), non ricostruiti a memoria, ma MAI
// verificati su un modulo M20/M20D reale. Da testare via log seriale
// prima di qualunque uso in campo, stessa prassi gia' seguita per gli
// altri chip GNSS di questo progetto.
esp_err_t gnss_bynav_configure_base(uart_port_t uart_num);

// Configura lo stesso ricevitore come rover: esce dalla modalita' base e
// abilita l'uscita NMEA GGA (usata da ntrip_rover_client.c per inoltrare
// la posizione al caster). Stesse avvertenze di gnss_bynav_configure_base
// sui comandi non verificati su hardware reale.
esp_err_t gnss_bynav_configure_rover(uart_port_t uart_num);

// Configura un ricevitore Bynav M21D (stesso pinout/comandi base di M20D,
// ma con IMU vero a bordo) come rover con navigazione INS attiva: braccio
// di leva delle due antenne e orientamento veicolo (da app_settings_t,
// vedi settings.h - valori da misurare fisicamente sull'installazione
// reale), piu' l'uscita dei log #INSPVAXA/#HEADINGA (letti da
// gnss_nmea_reader.c, vedi status_bynav_ins_get()) oltre al normale GGA.
// NON usare per M20D: quel modulo non ha IMU, i comandi INS non
// avrebbero effetto utile. Comandi presi da
// "AN065_X1_X2_M2_GNSS+INS Quick Start Guide" (ufficiale Bynav), MAI
// verificati su un modulo M21D reale (nessun hardware disponibile in
// questo progetto al momento della scrittura).
esp_err_t gnss_bynav_m21d_configure_rover(uart_port_t uart_num);
