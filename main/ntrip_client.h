#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// Consuma byte RTCM3 da rtcm_stream e li inoltra al caster EVONETRTK
// come sorgente NTRIP (SOURCE <password> /<mountpoint>). Si riconnette
// automaticamente in caso di errore. arg = StreamBufferHandle_t.
void ntrip_client_task(void *arg);

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Prova il collegamento come sorgente (base) con i parametri dati, senza
// salvarli: handshake SOURCE, poi chiude subito. out_msg riceve l'esito in
// una frase leggibile. Usato dal pulsante "Prova connessione al caster" del
// pannello in modalita' base.
bool ntrip_client_test_source(const char *host, uint16_t port, const char *mountpoint,
                              const char *password, char *out_msg, size_t out_msg_size);
