#pragma once

#include <stddef.h>
#include <stdint.h>

// Modalita' base: separa il flusso che arriva dal ricevitore in frame RTCM3
// (verificati con CRC24Q), righe NMEA e frame UBX. Solo i frame RTCM3 validi
// vanno al caster (callback on_rtcm); GGA e GSV finiscono nello stato fix e
// nel grafico satelliti (gnss_fix.c/gnss_signal.c), cosi' anche in base si
// vede cosa riceve l'antenna; i frame UBX (risposte ai comandi) vengono
// saltati per intero.
typedef void (*base_demux_rtcm_cb_t)(const uint8_t *frame, size_t len);

void base_stream_demux_feed(const uint8_t *buf, size_t len, base_demux_rtcm_cb_t on_rtcm);
