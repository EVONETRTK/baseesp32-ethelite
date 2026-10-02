#pragma once

#include <stdint.h>
#include <stdbool.h>

// Stima del traffico dati verso il caster, separata per WiFi e cellulare,
// per non restare senza dati sulla SIM. Conta i byte delle connessioni
// NTRIP (sono quasi tutto il traffico della base) piu' una stima delle
// intestazioni TCP/IP per ogni invio. Totali del giorno e del mese, salvati
// in NVS ogni 10 minuti (sopravvivono ai riavvii; si perdono al massimo gli
// ultimi 10 minuti se manca la corrente).

// sent = true per i dati in uscita. Usa la rete attiva in quel momento.
void data_usage_add(uint32_t bytes, bool sent);

typedef struct {
    char month[16];         // "2026-10", vuoto se l'ora non e' ancora nota
    uint64_t cell_month;
    uint64_t cell_day;
    uint64_t wifi_month;
    uint64_t wifi_day;
} data_usage_t;

data_usage_t data_usage_get(void);

void data_usage_start(void);
