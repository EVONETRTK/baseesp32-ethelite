#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Controllo dello spostamento dell'antenna della base, indipendente dal
// ricevitore in modalita' base.
//
// In modalita' base il ricevitore non calcola piu' la sua posizione: usa
// quella fissa (survey-in o coordinate inserite) e la trasmette nel 1005,
// che resta uguale finche' non si riavvia. Se l'antenna viene urtata o
// spostata nessuno se ne accorge, e i rover ricevono correzioni sbagliate.
//
// Qui la posizione viene ricalcolata sull'ESP32 dalle misure grezze dello
// ZED (UBX RXM-RAWX, pseudodistanze) e dai dati di navigazione dei satelliti
// (UBX RXM-SFRBX: effemeridi GPS LNAV e Galileo I/NAV, modello di ionosfera
// Klobuchar): posizione singola L1/E1 ogni 5 s, mediana su 30 minuti,
// confrontata con la posizione trasmessa nel 1005. Precisione della mediana
// circa 1 m a cielo libero (2-3 m su un balcone, provato il 04/10/2026 con
// una registrazione reale): l'avviso scatta solo con lo spostamento oltre la
// soglia (orizzontale, o verticale oltre il doppio) per due finestre di fila.
//
// Solo con ricevitori u-blox in modalita' base.

#define BASE_SELFPOS_OFF       0  // non disponibile (rover o ricevitore non u-blox)
#define BASE_SELFPOS_WAIT_SATS 1  // in attesa di dati di navigazione / satelliti sufficienti
#define BASE_SELFPOS_WAIT_1005 2  // la base non trasmette ancora la sua posizione
#define BASE_SELFPOS_RUNNING   3  // confronto in corso

typedef struct {
    uint8_t state;
    uint8_t sats;             // satelliti usati nell'ultima soluzione
    uint8_t eph_gps, eph_gal; // effemeridi valide
    uint16_t samples;         // soluzioni raccolte nella finestra in corso
    uint32_t window_elapsed_s;
    uint32_t window_len_s;
    bool have_result;         // almeno una finestra completa
    float d_east_m, d_north_m, d_up_m; // ultima finestra: posizione calcolata - posizione trasmessa
    float horiz_m;
    uint16_t result_samples;
    uint32_t result_age_s;
    uint32_t windows;         // finestre complete dall'avvio
    uint8_t exceed;           // finestre di fila oltre la soglia
    bool alarm;               // spostamento confermato
    float threshold_m;
} base_selfpos_status_t;

// Da chiamare una volta all'avvio in modalita' base, con il ricevitore
// effettivo e la modalita' della posizione (coordinate fisse in ETRF2000 o
// survey-in del ricevitore).
void base_selfpos_start(bool ublox_base, bool fixed_coords_etrf2000);

// Flusso grezzo del ricevitore (stesso di raw_log_feed): prende solo i
// frame RXM-RAWX e RXM-SFRBX e li passa al task di calcolo.
void base_selfpos_feed(const uint8_t *buf, size_t len);

// Uscita RXM-RAWX richiesta al ricevitore (ogni quante epoche, 0 = nessuna).
uint8_t base_selfpos_wanted_rate(void);

void base_selfpos_get_status(base_selfpos_status_t *out);
