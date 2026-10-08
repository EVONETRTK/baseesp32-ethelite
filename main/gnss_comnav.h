#pragma once

#include <stdbool.h>
#include "driver/uart.h"
#include "esp_err.h"

// Ricevitori ComNav / SinoGNSS (K922 e serie K): comandi ASCII in stile
// NovAtel, dal "ComNav OEM Board Reference Manual" V1.8 (2019, sito
// comnavtech.com): UNLOGALL, FIX POSITION / FIX AUTO, LOG <msg> ONTIME <s>
// (messaggi RTCM3 con la B finale, es. RTCM1074B), UNDULATION, SAVECONFIG,
// LOG VERSIONA. Il manuale e' precedente al K922: comandi da verificare sul
// modulo vero (il log di avvio mostra ogni comando mandato).
//
// INS e prua dal "K-series OEM Board User Guide" (cap. 4.6 e 5.8, per i
// moduli K803/K823 con IMU): INSCONTROL, SET IMUAXESTYPE, LOG HEADINGA.
// NON ancora fatto, perche' manca nei manuali: il comando per attivare
// Galileo HAS sul K922.
// La soluzione HAS, quando attiva, si legge dal log BESTPOSA (stessi campi
// del PPPNAVA Unicore: vedi gnss_unicore_ppp_get).

esp_err_t gnss_comnav_configure_base(uart_port_t uart_num);
esp_err_t gnss_comnav_configure_rover(uart_port_t uart_num);

// Righe ASCII con '#' dal ricevitore: #VERSIONA per modello e firmware.
void gnss_comnav_note_line(const char *line);

// Modello e firmware letti da VERSIONA ("" se non letti).
const char *gnss_comnav_model(void);
const char *gnss_comnav_fw(void);

// La risposta a VERSIONA sembra di un ComNav (modello K8xx/K9xx o nome ComNav/SinoGNSS).
bool gnss_comnav_looks_comnav(void);
