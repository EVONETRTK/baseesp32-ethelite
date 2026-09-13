#pragma once

#include "driver/uart.h"
#include "esp_err.h"

// Configura un ricevitore Unicore (es. UM980/UM982) collegato sulla UART
// indicata come base RTK: auto-survey e uscita RTCM3 sulla stessa porta.
//
// ATTENZIONE: i comandi ASCII usati qui (vedi gnss_unicore.c) sono
// ricostruiti dalla documentazione pubblica della famiglia Unicore UM98x,
// senza possibilita' di verifica su hardware reale in questo progetto
// (ESP-IDF non installato). Vanno controllati contro il manuale del
// modulo specifico prima dell'uso in campo.
esp_err_t gnss_unicore_configure_base(uart_port_t uart_num);

// Configura lo stesso ricevitore come rover: esce dalla modalita' base e
// abilita l'uscita NMEA GGA (usata da ntrip_rover_client.c per inoltrare
// la posizione al caster). Stesse avvertenze di
// gnss_unicore_configure_base sui comandi non verificati.
esp_err_t gnss_unicore_configure_rover(uart_port_t uart_num);
