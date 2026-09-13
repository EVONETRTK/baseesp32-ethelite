#pragma once

#include <stddef.h>
#include "driver/uart.h"

// Si connette al caster EVONETRTK come client NTRIP (richiesta GET sulla
// mountpoint, autenticazione Basic con ntrip_username/ntrip_password),
// riceve il flusso RTCM3 e lo scrive in tempo reale sulla UART verso il
// ricevitore GNSS. Si riconnette automaticamente in caso di errore.
// arg = uart_port_t incapsulato come (void *)(intptr_t) uart_num.
void ntrip_rover_client_task(void *arg);

// Chiamata da gnss_nmea_reader quando intercetta una riga $GxGGA emessa
// dal GNSS: se il client e' attualmente connesso al caster, la inoltra
// sullo stesso socket (prassi NTRIP standard per comunicare la posizione
// del rover). Se non connesso, la riga viene scartata silenziosamente.
// line non deve includere il terminatore CR/LF.
void ntrip_rover_client_forward_gga(const char *line, size_t len);
