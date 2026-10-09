#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Misura della posizione della base, due modi:
// - RTK: la base si riavvia come rover collegata al mountpoint del rover
//   (es. una stazione della rete), fa la media delle posizioni con fix RTK,
//   salva le coordinate come posizione fissa e torna base. Precisione
//   centimetrica, nel sistema della rete che manda le correzioni.
// - Galileo HAS: il ricevitore (Unicore UM98x) calcola il PPP con le
//   correzioni HAS ricevute dai satelliti (segnale E6), senza caster ne'
//   internet; media per ore, conversione da ITRF2020 a ETRF2000 (etrf.h),
//   poi posizione fissa. Precisione di circa un decimetro.

#define BASE_MEASURE_RTK  0
#define BASE_MEASURE_HAS  1

typedef struct {
    bool active;          // misura in corso (dispositivo in modalita' rover)
    uint8_t method;       // BASE_MEASURE_RTK / BASE_MEASURE_HAS
    uint32_t elapsed_s;   // dall'inizio della misura in questo avvio
    uint32_t fixed_n;     // RTK: campioni con fix fisso; HAS: soluzioni HAS buone (PPPNAVA)
    uint32_t float_n;     // RTK: campioni float; HAS: tutte le righe PPPNAVA
    uint32_t target_n;    // campioni fissi necessari (RTK)
    uint32_t timeout_s;   // tempo massimo (RTK)
    uint32_t has_done_s;        // HAS: secondi di misura utili, sommati sugli avvii
    uint32_t has_target_s;      // HAS: durata richiesta
    uint32_t has_warmup_left_s; // HAS: convergenza ancora da attendere in questo avvio
    uint32_t has_sigma_cm;      // HAS: incertezza orizzontale dichiarata dell'ultima soluzione (0 = nessuna)
    uint32_t has_corr_age_s;    // HAS: eta' delle correzioni
    int quality;          // qualita' dell'ultimo fix (GGA), -1 se nessuno
} base_measure_progress_t;

// Avvia la misura RTK: salva lo stato e la modalita' rover, poi riavvia.
// false (con motivo in err) se non si puo' (es. mountpoint rover vuoto).
bool base_measure_request_start(char *err, size_t err_size);

// Avvia la misura con Galileo HAS per hours ore (1-24). Solo Unicore.
bool base_measure_request_start_has(int hours, char *err, size_t err_size);

// Annulla la misura in corso: torna base senza toccare le coordinate.
void base_measure_request_cancel(void);

// Misura HAS in corso (letta all'avvio da main.c e dal driver Unicore).
bool base_measure_is_has(void);

// Misura della posizione in corso (salvata nelle impostazioni): vale gia' all'avvio,
// prima di base_measure_start_if_active().
bool base_measure_is_active(void);

// Da chiamare all'avvio: se una misura e' in corso parte il task che la fa.
void base_measure_start_if_active(void);

base_measure_progress_t base_measure_get_progress(void);
