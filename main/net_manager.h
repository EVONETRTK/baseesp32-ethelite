#pragma once

// Inizializza WiFi e (se abilitato in Kconfig) il modem cellulare SIM868,
// poi avvia un task di supervisione che sceglie automaticamente la rete
// disponibile - WiFi preferito, GPRS come fallback - e gestisce le
// riconnessioni quando il collegamento attivo cade. Blocca finche' non
// e' disponibile una connessione di rete funzionante.
void net_manager_start(void);
