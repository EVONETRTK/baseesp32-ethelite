#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

// Misura della posizione della base con RTK: la base si riavvia come rover
// collegata al mountpoint del rover (es. una stazione della rete), fa la
// media delle posizioni con fix RTK, salva le coordinate come posizione
// fissa e torna base. Precisione centimetrica invece dei metri del
// survey-in, senza cercare le coordinate altrove.

typedef struct {
    bool active;          // misura in corso (dispositivo in modalita' rover)
    uint32_t elapsed_s;   // dall'inizio della misura in questo avvio
    uint32_t fixed_n;     // campioni con fix RTK fisso raccolti
    uint32_t float_n;     // campioni con fix RTK float raccolti
    uint32_t target_n;    // campioni fissi necessari
    uint32_t timeout_s;   // tempo massimo
    int quality;          // qualita' dell'ultimo fix (GGA), -1 se nessuno
} base_measure_progress_t;

// Avvia la misura: salva lo stato e la modalita' rover, poi riavvia.
// false (con motivo in err) se non si puo' (es. mountpoint rover vuoto).
bool base_measure_request_start(char *err, size_t err_size);

// Annulla la misura in corso: torna base senza toccare le coordinate.
void base_measure_request_cancel(void);

// Da chiamare all'avvio: se una misura e' in corso parte il task che la fa.
void base_measure_start_if_active(void);

base_measure_progress_t base_measure_get_progress(void);
