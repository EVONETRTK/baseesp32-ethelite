#pragma once

#include <stddef.h>
#include <stdbool.h>
#include <stdint.h>
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

// Prova subito una connessione NTRIP con i parametri passati (non
// necessariamente ancora salvati in settings - stessa idea del "Connetti"
// gia' usato per il WiFi: testare senza dover salvare e riavviare per
// scoprire se sono giusti). Si connette, fa l'handshake GET+Basic Auth,
// legge la prima risposta e chiude subito - non tocca lo stato NTRIP
// "live" mostrato altrove nella UI (ntrip_status.h), e' solo un test
// puntuale. Scrive un messaggio leggibile in out_msg in ogni caso (successo
// o fallimento) e ritorna true solo se il caster ha accettato la richiesta.
bool ntrip_rover_client_test_connect(const char *host, uint16_t port, const char *mountpoint,
                                      const char *username, const char *password,
                                      char *out_msg, size_t out_msg_size);

// Chiamata da gnss_nmea_reader quando intercetta una riga $GxGGA emessa
// dal GNSS: se il client e' attualmente connesso al caster, la inoltra
// sullo stesso socket (prassi NTRIP standard per comunicare la posizione
// del rover). Se non connesso, la riga viene scartata silenziosamente.
// line non deve includere il terminatore CR/LF.
void ntrip_rover_client_forward_gga(const char *line, size_t len);
