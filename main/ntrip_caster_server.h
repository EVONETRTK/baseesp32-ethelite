#pragma once

#include <stdint.h>
#include <stddef.h>

// Avvia il server caster NTRIP locale (se abilitato in settings, campo
// ntrip_caster_server_enable) - accetta connessioni da rover sulla stessa
// rete (o raggiungibili da internet solo se l'utente ha configurato il
// port forwarding sul proprio router: impossibile in pratica sui dati del
// modem cellulare, quasi sempre dietro NAT condiviso dall'operatore) e
// inoltra loro il flusso RTCM3 generato dal GNSS in modalita' base. Va
// chiamata una sola volta all'avvio, solo in modalita' base; non fa nulla
// se il server e' disattivato in settings.
void ntrip_caster_server_start(void);

// Da chiamare ogni volta che arrivano nuovi byte RTCM3 dal GNSS (stesso
// stream gia' inoltrato al caster esterno) - non bloccante (scrive solo
// su uno stream buffer interno): il vero invio ai rover collegati avviene
// in un task separato, per non rallentare il chiamante se un client e'
// lento a leggere.
void ntrip_caster_server_feed(const uint8_t *data, size_t len);

// Numero di rover attualmente collegati al server caster locale.
size_t ntrip_caster_server_get_client_count(void);
