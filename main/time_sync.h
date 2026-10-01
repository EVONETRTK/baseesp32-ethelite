#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Ora vera via NTP (pool.ntp.org), fuso orario italiano. Prima la base
// conosceva solo i secondi dall'accensione: nei log non si sapeva quando
// fosse successo qualcosa (es. la temperatura massima del pomeriggio).
void time_sync_start(void);

// true dopo la prima sincronizzazione riuscita.
bool time_sync_is_valid(void);

// Ora locale "2026-10-01 15:32:05", o "" se non ancora sincronizzata.
void time_sync_format_now(char *out, size_t out_size);

// Come sopra per un istante esp_timer (us dall'avvio) del passato.
void time_sync_format_uptime(int64_t uptime_us, char *out, size_t out_size);
