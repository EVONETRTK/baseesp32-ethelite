#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Ora vera via NTP (pool.ntp.org), fuso orario italiano. Prima la base
// conosceva solo i secondi dall'accensione: nei log non si sapeva quando
// fosse successo qualcosa (es. la temperatura massima del pomeriggio).
void time_sync_start(void);

// Solo il fuso orario: da chiamare all'inizio dell'avvio, cosi' anche le
// prime righe del log usano l'ora locale (prima erano 2 ore indietro).
void time_sync_set_timezone(void);

// true dopo la prima sincronizzazione riuscita (NTP o satelliti).
bool time_sync_is_valid(void);

// Ora UTC dal ricevitore GNSS (UBX NAV-TIMEUTC in base, NMEA RMC in rover).
// Usata solo finche' l'NTP non ha mai risposto: senza internet (guasto IPv4
// del 04/10/2026) la base restava per ore senza ora, e VPN e registrazione
// dei dati grezzi aspettavano. L'NTP, quando arriva, ha la precedenza.
void time_sync_from_gnss(int year, int month, int day, int hour, int min, int sec);

// Fonte dell'ora attuale: "NTP", "GNSS" o "" se non ancora valida.
const char *time_sync_source(void);

// true se l'ora non e' ancora arrivata dall'NTP (va chiesta al ricevitore).
bool time_sync_wants_gnss(void);

// Ora locale "2026-10-01 15:32:05", o "" se non ancora sincronizzata.
void time_sync_format_now(char *out, size_t out_size);

// Come sopra per un istante esp_timer (us dall'avvio) del passato.
void time_sync_format_uptime(int64_t uptime_us, char *out, size_t out_size);
