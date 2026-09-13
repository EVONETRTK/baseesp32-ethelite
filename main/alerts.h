#pragma once

#include <stdbool.h>
#include <stddef.h>
#include "settings.h"

// Avvia il task che controlla periodicamente lo stato della connessione al
// caster NTRIP (status_ntrip_get()) e invia un avviso (email e/o WhatsApp,
// secondo cosa e' configurato in app_settings_t) se resta disconnessa
// oltre alert_threshold_min minuti, piu' un secondo avviso quando torna a
// funzionare. Va chiamata una volta all'avvio.
void alerts_start(void);

// Invia subito un avviso di prova sui canali configurati in s (usato dal
// pulsante "Invia avviso di prova" della UI web, con i valori correnti del
// form - non serve averli gia' salvati) - bloccante, puo' richiedere
// qualche secondo (handshake TLS per l'email). Ritorna true se ALMENO un
// canale configurato ha avuto successo; out_msg (se non NULL) riceve un
// riepilogo leggibile dell'esito di ciascun canale.
bool alerts_send_test(const app_settings_t *s, char *out_msg, size_t out_msg_size);
