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

// "Battito" dei task principali: ognuno lo chiama a ogni giro del suo ciclo.
// Se un task che ha gia' battuto almeno una volta tace oltre il suo limite,
// la sorveglianza riavvia la base (task bloccato senza crash). La base e'
// spesso lontana: meglio un riavvio che restare viva ma ferma.
typedef enum {
    HB_NET = 0,     // gestione della rete (net_manager)
    HB_NTRIP,       // client NTRIP (base o rover)
    HB_GNSS,        // lettura del ricevitore
    HB_ALERTS,      // avvisi, SIM, monitoraggio remoto
    HB_COUNT
} heartbeat_t;

void sys_stats_heartbeat(heartbeat_t which);

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

// true se questo avvio segue un guasto: crash, watchdog, oppure un riavvio di
// sicurezza (task bloccato, ricevitore muto...). Non per accensione, pulsante
// di reset o riavvio voluto. Valida dopo sys_stats_boot_report().
bool sys_stats_boot_was_fault(void);

// Dettagli dell'ultimo crash (motivo e indirizzi), salvati dal gestore dei
// crash e scritti nel log all'avvio. Stringa vuota se l'avvio non segue un crash.
void sys_stats_crash_report(void);
const char *sys_stats_last_crash(void);
