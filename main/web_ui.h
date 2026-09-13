#pragma once

// Avvia il server web di configurazione/stato. Va chiamato dopo
// net_manager_start() (che porta su l'AP di setup e tenta WiFi/GPRS).
// La UI resta raggiungibile sull'IP dell'AP (192.168.4.1 di default)
// indipendentemente dallo stato di WiFi/cellulare, e anche sull'IP della
// rete STA/GPRS una volta connessi.
void web_ui_start(void);
