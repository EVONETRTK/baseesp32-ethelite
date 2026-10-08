#pragma once

#include <stddef.h>
#include "settings.h"

// Impostazioni che valgono solo dopo un riavvio (quelle con "Salva e
// riavvia" nel pannello): all'avvio se ne tiene una copia; se poi vengono
// salvate diverse, il pannello mostra una barra fissa con il pulsante
// Riavvia. Prima l'avviso compariva solo per un attimo sotto il pulsante e
// la base continuava a usare i valori vecchi senza che si vedesse (caso del
// 08/10/2026: ricevitore cambiato in ComNav e poi rimesso u-blox senza
// riavvio).

// Da chiamare una volta all'avvio con le impostazioni lette dalla memoria,
// PRIMA che il riconoscimento del ricevitore modifichi la copia di avvio.
void pending_restart_snapshot(const app_settings_t *s);

// Elenco, in parole, delle impostazioni salvate diverse da quelle in uso
// (es. "ricevitore GNSS, posizione della base"); stringa vuota se nessuna.
void pending_restart_describe(const app_settings_t *s, char *out, size_t out_size);
