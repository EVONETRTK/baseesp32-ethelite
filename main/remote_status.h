#pragma once

#include "settings.h"

// Monitoraggio remoto: ogni remote_interval_min minuti la base manda un
// riassunto del suo stato (JSON) con un POST a remote_url. Chiamata ogni
// minuto dal task degli avvisi (alerts.c), che ha lo stack per l'HTTPS.
void remote_status_tick(const app_settings_t *cfg);

// Esito dell'ultimo invio, per il pannello ("" se mai inviato).
const char *remote_status_last_result(void);
