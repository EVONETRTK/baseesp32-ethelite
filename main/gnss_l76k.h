#pragma once

#include "driver/uart.h"
#include "esp_err.h"

// Quectel L76K: il GPS di navigazione montato su tutti gli shield LilyGO
// della T-ETH-Elite (LTE, Gateway, LoRa), collegato ai pin GPS 39/42 della
// scheda tramite il DIP "GPS" dello shield. NON e' un ricevitore RTK: niente
// modalita' base, niente uscita RTCM, precisione di qualche metro. Serve solo
// per provare la catena rover (lettura NMEA, satelliti, client NTRIP, GGA al
// caster, uscita UDP) senza il ricevitore RTK vero.
//
// Comandi $PCAS (protocollo CASIC del chip AT6558 su cui e' basato l'L76K),
// mandati a ogni avvio perche' non vengono salvati nel modulo.

// Non supportato: logga un errore e non manda nulla al modulo.
esp_err_t gnss_l76k_configure_base(uart_port_t uart_num);

// Attiva GPS + BeiDou + GLONASS ($PCAS04,7). Di fabbrica l'L76K usa solo
// GPS + BeiDou.
esp_err_t gnss_l76k_configure_rover(uart_port_t uart_num);
