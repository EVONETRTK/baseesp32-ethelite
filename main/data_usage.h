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
    uint32_t counted_s;     // secondi contati in questo mese (0 = sconosciuto)
} data_usage_t;

data_usage_t data_usage_get(void);

void data_usage_start(void);

// Storico dei giorni passati (oggi escluso), per il grafico degli ultimi 30.
#define DATA_USAGE_HISTORY_DAYS 30
typedef struct {
    uint32_t day;      // aaaammgg
    uint32_t cell_kb;
    uint32_t wifi_kb;
} data_usage_day_t;

// Copia in out gli ultimi giorni, dal piu' vecchio; restituisce quanti.
int data_usage_get_history(data_usage_day_t *out, int max);
