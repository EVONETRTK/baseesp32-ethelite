#pragma once

#include <stdbool.h>

// Avvia il server web di configurazione/stato. Va chiamato dopo
// net_manager_start() (che porta su l'AP di setup e tenta WiFi/GPRS).
// La UI resta raggiungibile sull'IP dell'AP (192.168.4.1 di default)
// indipendentemente dallo stato di WiFi/cellulare, e anche sull'IP della
// rete STA/GPRS una volta connessi.
void web_ui_start(void);

// true se un test di connessione WiFi avviato dalla UI (pulsante "Connetti")
// e' attualmente in corso. net_manager_task lo controlla prima di lanciare
// un proprio tentativo di riconnessione automatica: senza questo controllo,
// una disconnessione causata DAL test stesso (passaggio a una rete diversa)
// veniva letta come "rete persa" e faceva ripartire la riconnessione
// automatica verso la rete nota migliore - che spesso vinceva la corsa e
// riportava il dispositivo sulla rete vecchia subito dopo un test riuscito
// verso quella nuova, annullando la scelta esplicita dell'utente.
bool web_ui_wifi_test_in_progress(void);
