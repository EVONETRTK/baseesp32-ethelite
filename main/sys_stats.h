#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef struct {
    uint32_t free_heap_bytes;
    uint32_t min_free_heap_bytes; // minimo storico dal boot (indicatore di eventuali perdite di memoria)
    uint32_t total_heap_bytes;    // heap interna (DRAM), esclusa PSRAM
    bool psram_present;
    uint32_t free_psram_bytes;
    uint32_t total_psram_bytes;
    float cpu0_percent;           // -1 se non ancora disponibile (serve un secondo campione)
    float cpu1_percent;           // -1 se non ancora disponibile o scheda mono-core
    float chip_temp_c;            // temperatura interna del chip (°C), -1000 se il sensore non e' disponibile
} sys_stats_t;

// Legge lo stato corrente di memoria e uso CPU. L'uso CPU e' calcolato come
// differenza tra due chiamate successive (tempo di esecuzione del task
// IDLE di ciascun core nell'intervallo) - la primissima chiamata dall'avvio
// restituisce -1 per i campi cpuN_percent, in attesa di un secondo
// campione di riferimento.
sys_stats_t sys_stats_get(void);
