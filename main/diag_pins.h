#pragma once

#include "cJSON.h"

// Diagnosi della seriale del ricevitore (GET /api/diag/gnss_pins): come sono
// instradati davvero i pin TX/RX dentro il chip (IO_MUX, matrice GPIO,
// abilitazione dell'uscita) e quante commutazioni si vedono sui due pin in
// una finestra breve, mentre la base manda una raffica di prova sulla
// seriale. Serve a capire dove si perde il segnale quando il ricevitore non
// risponde in seriale (trovato il 10/10/2026 con la Syneda uRTK6).
void diag_pins_gnss(cJSON *root, int tx_pin, int rx_pin);

// Uso della CPU di ogni task in un secondo (GET /api/diag/tasks).
void diag_tasks(cJSON *root);
