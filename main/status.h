#pragma once

#include <stdint.h>

typedef enum {
    NET_STATUS_NONE,
    NET_STATUS_WIFI,
    NET_STATUS_CELLULAR,
} net_status_t;

// Stato condiviso, letto dalla UI web e aggiornato dagli altri task.
// Non e' protetto da lock: le singole letture/scritture sono variabili
// scalari, sufficiente per un pannello di stato non critico.

void status_set_net(net_status_t s);
net_status_t status_get_net(void);

void status_note_rtcm_bytes(uint32_t n);
uint32_t status_get_rtcm_total_bytes(void);

// Timestamp (microsecondi, esp_timer_get_time) dell'ultimo dato RTCM
// ricevuto dalla UART GNSS; 0 se non ancora ricevuto nulla.
int64_t status_get_last_rtcm_time_us(void);
