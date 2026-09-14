#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Da chiamare ogni volta che arrivano nuovi byte RTCM3 grezzi dal
// ricevitore in modalita' base (stesso stream gia' inoltrato al caster) -
// analizza lo stream alla ricerca di frame di posizione (1005/1006) e
// aggiorna lo stato condiviso. La prima posizione trovata dopo l'avvio
// diventa il riferimento; le successive vengono confrontate con quella
// per calcolare lo spostamento. Non invia da sola nessun avviso
// (operazione bloccante - HTTP/SMTP - non adatta a un task che deve
// restare reattivo sulla UART): vedi alerts.c, che interroga
// base_monitor_get_status() periodicamente e decide se avvisare.
void base_monitor_feed(const uint8_t *data, size_t len);

typedef struct {
    bool baseline_set;
    double drift_m; // distanza dalla posizione di riferimento (metri), valida solo se baseline_set

    // Ultima posizione (ECEF) riportata dal ricevitore, aggiornata ad ogni
    // frame 1005/1006 valido (a differenza della baseline sopra, che resta
    // fissa al primo frame dopo il boot) - usata dalla UI web per proporre
    // le coordinate da confermare quando si passa a posizione base manuale
    // (vedi settings.h, base_position_mode), cosi' l'utente non deve
    // trascriverle a mano da un'altra fonte.
    bool last_position_set;
    double last_ecef_x_m;
    double last_ecef_y_m;
    double last_ecef_z_m;
} base_monitor_status_t;

base_monitor_status_t base_monitor_get_status(void);
