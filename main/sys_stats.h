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
    float chip_temp_max_c;        // massima dall'accensione (°C), -1000 se non disponibile
    int64_t chip_temp_max_us;     // quando e' stata raggiunta la massima (esp_timer, us)
} sys_stats_t;

// Legge lo stato corrente di memoria e uso CPU. L'uso CPU e' calcolato come
// differenza tra due chiamate successive (tempo di esecuzione del task
// IDLE di ciascun core nell'intervallo) - la primissima chiamata dall'avvio
// restituisce -1 per i campi cpuN_percent, in attesa di un secondo
// campione di riferimento.
sys_stats_t sys_stats_get(void);

// Avvia la sorveglianza in background di CPU e memoria (avvisi nel log).
void sys_stats_monitor_start(void);

// Soglia oltre la quale la temperatura del chip e' considerata alta (log e avvisi).
#define SYS_TEMP_ALERT_C 70.0f
#define SYS_TEMP_REARM_C (SYS_TEMP_ALERT_C - 5.0f) // isteresi: nuovo avviso solo dopo essere sceso sotto

// Motivo dell'ultimo riavvio, in parole semplici (es. "calo di tensione
// dell'alimentazione", "riavvio software: richiesto dal pannello").
// sys_stats_boot_report() va chiamata una volta all'avvio.
void sys_stats_boot_report(void);
const char *sys_stats_last_reset(void);

// Riavvia annotando il motivo, che al prossimo avvio compare nel log e nel
// pannello. La seconda variante annota soltanto (riavvio fatto dal chiamante).
void sys_stats_restart_with_reason(const char *reason);
void sys_stats_note_restart_reason(const char *reason);
