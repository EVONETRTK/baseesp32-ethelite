#pragma once

#include <stdint.h>

// Salvavita dell'avvio e traccia dei passi (09/10/2026).
//
// Il 09/10 la base, dopo il riavvio di un aggiornamento, e' rimasta ferma per
// minuti senza scrivere nemmeno una riga di log: nella prima parte dell'avvio
// nessun watchdog fa ripartire la scheda (quello dei task e' configurato piu'
// avanti, in sys_stats_monitor_start).
//
// - Il watchdog RTC del chip, acceso dal bootloader, resta acceso anche nel
//   firmware (CONFIG_BOOTLOADER_WDT_DISABLE_IN_USER_CODE) e viene portato a
//   BOOT_GUARD_TIMEOUT_S con reset del sistema digitale (CPU e periferiche;
//   WDT_STAGE_ACTION_RESET_SYSTEM): se l'avvio non arriva a boot_guard_done()
//   la scheda riparte da sola. Non e' un reset completo: la parte RTC non
//   riparte (la memoria RTC resta, i pin di avvio non vengono riletti) e i
//   dispositivi esterni (ricevitore GNSS, modem) restano accesi.
// - Ogni passo dell'avvio e' annotato in memoria RTC (sopravvive ai riavvii
//   software e ai watchdog) e, nei punti principali, nella NVS (sopravvive
//   anche a un calo di corrente): al riavvio successivo il log dice dove si
//   era fermato l'avvio precedente.

#define BOOT_GUARD_TIMEOUT_S 120

typedef enum {
    BOOT_STAGE_START = 1,
    BOOT_STAGE_NVS,
    BOOT_STAGE_SETTINGS,
    BOOT_STAGE_OLED,
    BOOT_STAGE_LICENSE,
    BOOT_STAGE_GNSS_PROBE,
    BOOT_STAGE_NET,
    BOOT_STAGE_WEB,
    BOOT_STAGE_SD_UPDATE,
    BOOT_STAGE_GNSS_DETECT,
    BOOT_STAGE_SERVICES,
    BOOT_STAGE_DONE,
} boot_stage_t;

// Primissima cosa in app_main: allunga il watchdog RTC e legge la traccia
// dell'avvio precedente rimasta in memoria RTC.
void boot_guard_early(void);

// Dopo nvs_flash_init(): legge dalla NVS l'ultimo passo dell'avvio precedente
// e lo scrive nel log se l'avvio non era finito.
void boot_guard_after_nvs(void);

void boot_guard_stage(boot_stage_t stage);

// Fine dell'avvio: spegne il watchdog RTC (da qui vale quello dei task).
void boot_guard_done(void);

// Prima di un riavvio voluto (chiamata da sys_stats_note_restart_reason):
// segna l'avvio come completato, in memoria RTC e nella NVS, cosi' un riavvio
// a meta' avvio (aggiornamento da microSD, comando dal pannello) non lascia
// un falso "avvio precedente fermo a".
void boot_guard_restarting(void);

// Millisecondi prima che scatti il salvavita dell'avvio (UINT32_MAX ad avvio finito).
uint32_t boot_guard_remaining_ms(void);

// Testo per il pannello: dove si e' fermato l'avvio precedente ("" se finito).
const char *boot_guard_prev_note(void);
