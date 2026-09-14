#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

typedef struct {
    double ecef_x_m;
    double ecef_y_m;
    double ecef_z_m;
} rtcm3_position_t;

// Inizializza lo stato interno del parser (buffer di sincronizzazione del
// frame RTCM3). Va chiamata una volta prima del primo uso di
// rtcm3_1005_feed(), o per azzerarlo esplicitamente (es. a ogni avvio).
void rtcm3_1005_init(void);

// Analizza un blocco di byte RTCM3 grezzi, tipicamente un frammento dello
// stream continuo generato dal ricevitore in modalita' base - puo' essere
// chiamata ripetutamente con frammenti consecutivi dello stesso stream
// (es. ogni volta che main.c legge dalla UART GNSS), mantiene lo stato di
// sincronizzazione tra una chiamata e l'altra. Cerca frame validi
// (preambolo 0xD3 + CRC24Q verificato) di tipo 1005 o 1006 (posizione
// ECEF dell'antenna, con o senza altezza - l'altezza viene ignorata,
// serve solo la posizione per rilevare uno spostamento) in tutto il
// blocco passato. Se ne trova almeno uno, scrive in *out_pos la posizione
// dell'ULTIMO trovato in questa chiamata e ritorna true (per rilevare uno
// spostamento serve solo la posizione piu' recente, non la storia
// completa).
bool rtcm3_1005_feed(const uint8_t *data, size_t len, rtcm3_position_t *out_pos);
