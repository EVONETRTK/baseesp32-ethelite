#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

// Da chiamare ogni volta che arrivano nuovi byte RTCM3 grezzi dal
// ricevitore in modalita' base (stesso stream gia' inoltrato al caster) -
// analizza lo stream alla ricerca di frame di posizione (1005/1006) e
// aggiorna lo stato condiviso con l'ultima posizione trovata. Non invia da
// sola nessun avviso (operazione bloccante - HTTP/SMTP - non adatta a un
// task che deve restare reattivo sulla UART): vedi alerts.c, che interroga
// base_monitor_get_status() periodicamente e decide se avvisare. Lo
// spostamento dell'antenna lo controlla base_selfpos.c.
void base_monitor_feed(const uint8_t *data, size_t len);

typedef struct {
    // Ultima posizione (ECEF) riportata dal ricevitore, aggiornata ad ogni
    // frame 1005/1006 valido - usata dalla UI web per proporre le
    // coordinate da confermare quando si passa a posizione base manuale
    // (vedi settings.h, base_position_mode), cosi' l'utente non deve
    // trascriverle a mano da un'altra fonte, e da base_selfpos.c come
    // posizione di riferimento.
    bool last_position_set;
    double last_ecef_x_m;
    double last_ecef_y_m;
    double last_ecef_z_m;
    // Quando e' arrivato l'ultimo 1005/1006 (esp_timer, us; 0 = mai): una
    // base che non lo manda e' inutile ai rover, il pannello lo segnala.
    int64_t last_position_us;
} base_monitor_status_t;

base_monitor_status_t base_monitor_get_status(void);
