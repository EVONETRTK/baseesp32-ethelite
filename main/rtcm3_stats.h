#pragma once

#include <stdint.h>
#include <stddef.h>

// Conta quanti byte sono passati per ciascun numero di messaggio RTCM3
// osservato nello stream (base: dalla UART del GNSS) - solo diagnostica
// per la UI web ("quanto pesa davvero ciascun messaggio scelto"), non
// convalida il CRC (a differenza di rtcm3_1005.c, che invece lo fa perche'
// decodifica davvero il contenuto): un frame corrotto viene comunque
// attribuito al tipo che dichiara nell'header, l'imprecisione risultante
// e' accettabile per un contatore puramente informativo.
void rtcm3_stats_init(void);

// Va chiamata con lo stesso stream grezzo passato a base_monitor_feed()/
// ntrip_caster_server_feed() in main.c - può essere invocata piu' volte
// con spezzoni successivi dello stream, lo stato di scansione persiste
// tra una chiamata e l'altra.
void rtcm3_stats_feed(const uint8_t *data, size_t len);

typedef struct {
    uint16_t msg_type;
    uint32_t bytes;
} rtcm3_stat_entry_t;

// Scrive in out (fino a max_count voci) i tipi messaggio visti dal boot
// con almeno un frame completo, in qualunque ordine. Ritorna quante voci
// ha scritto.
size_t rtcm3_stats_get(rtcm3_stat_entry_t *out, size_t max_count);
