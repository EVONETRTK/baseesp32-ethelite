#pragma once

#include "driver/uart.h"
#include "esp_err.h"

// Configura un ricevitore Quectel LC29H (varianti BA/CA/DA/EA, non AA/LC79H)
// collegato sulla UART indicata come base RTK, e uscita RTCM3 MSM7 +
// posizione antenna (1005). Legge da settings.h (base_position_mode) se
// usare il survey-in automatico (60s, precisione richiesta 2,5m, stessa
// convenzione gia' usata per Unicore) oppure una posizione fissa nota
// (tipicamente da un servizio PPP, vedi settings.h) convertita in ECEF.
//
// I comandi ($PQTM..., $PAIR...) sono presi dalla documentazione ufficiale
// Quectel (LC29H Series&LC79H(AL) GNSS Protocol Specification V1.4), non da
// ricostruzione indiretta - ma senza possibilita' di verifica su hardware
// reale in questo progetto (modulo non ancora disponibile). Va confermato
// sul modulo fisico prima dell'uso in campo.
//
// ATTENZIONE (limite reale del chip, non di questo firmware): il cambio di
// modalita' base/rover (comando PQTMCFGRCVRMODE) "prende effetto solo dopo
// PQTMSAVEPAR e il riavvio del modulo" per esplicita nota del produttore -
// e su questa variante (a differenza di LC29H(AA)/LC79H(AL)) non esiste un
// comando software per riavviare il solo modulo GNSS. Cambiare modalita'
// richiede quindi uno spegnimento/riaccensione fisico del modulo (o
// dell'intero dispositivo, se il modulo GNSS perde alimentazione insieme
// all'ESP32) - riavviare solo il firmware (esp_restart/"Salva e riavvia")
// non basta.
esp_err_t gnss_lc29h_configure_base(uart_port_t uart_num);

// Configura lo stesso ricevitore come rover: modalita' rover (ripristina
// l'uscita NMEA standard, incluso $GxGGA usato da ntrip_rover_client.c per
// inoltrare la posizione al caster). Stesse avvertenze di
// gnss_lc29h_configure_base sopra.
esp_err_t gnss_lc29h_configure_rover(uart_port_t uart_num);
