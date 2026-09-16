#pragma once

#include <stddef.h>
#include <stdbool.h>
#include "driver/uart.h"

// Una voce "STR;" del sourcetable NTRIP del caster - vedi
// ntrip_rover_client_fetch_mountpoints() sotto.
typedef struct {
    char name[33];        // es. "AGRI_MSM4"
    char description[65]; // es. "Agricoltura standard (MSM4)"
} ntrip_mountpoint_entry_t;

#define NTRIP_MOUNTPOINTS_MAX 16

// Si collega a settings->ntrip_host:ntrip_port (nessuna autenticazione,
// il sourcetable NTRIP e' sempre pubblico per definizione del protocollo)
// e ne legge/analizza le righe "STR;...", riempiendo out con le mountpoint
// disponibili - richiesto dall'utente: prima si doveva scrivere a mano il
// nome esatto di una mountpoint senza sapere quali esistessero davvero ne'
// quali avessero una base attiva, l'esatto contrario di come funziona di
// solito un client NTRIP (che propone l'elenco, non lo fa indovinare).
// Ritorna il numero di voci scritte in out (fino a max_count), 0 se la
// connessione o la lettura falliscono.
size_t ntrip_rover_client_fetch_mountpoints(ntrip_mountpoint_entry_t *out, size_t max_count);

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
