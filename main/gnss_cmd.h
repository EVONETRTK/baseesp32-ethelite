#pragma once

#include "esp_err.h"

// Invio dei comandi di configurazione ai ricevitori GNSS (sopra gnss_io.c),
// in comune tra i driver dei chip. Ogni comando va nel log con il tag del
// driver che lo manda (tag) ed e' seguito da una pausa di 100 ms, per dare
// al ricevitore il tempo di elaborarlo prima del successivo.

// Comando ASCII Unicore/ComNav/Bynav: manda cmd seguito da "\r\n".
// ESP_OK se il ricevitore ha accettato tutto cmd, altrimenti ESP_FAIL.
esp_err_t gnss_cmd_send_line(const char *tag, const char *cmd);

// Comando proprietario NMEA (Quectel $PQTM/$PAIR, CASIC $PCAS): body e' il
// comando SENZA '$' iniziale ne' '*checksum' finale, es. "PCAS04,7"; manda
// "$<body>*<checksum>\r\n". ESP_ERR_INVALID_SIZE (niente inviato) se la riga
// non sta nel buffer, ESP_FAIL se il ricevitore non l'ha accettata tutta.
esp_err_t gnss_cmd_send_nmea(const char *tag, const char *body);
