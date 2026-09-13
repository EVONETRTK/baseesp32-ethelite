#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NET_STATUS_NONE,
    NET_STATUS_WIFI,
    NET_STATUS_CELLULAR,
} net_status_t;

// Stato della connessione al caster NTRIP (base: carica RTCM; rover:
// scarica RTCM) - separato dal conteggio byte RTCM sopra, che dice se
// arrivano dati dalla UART GNSS ma non se il caster stesso e' raggiungibile.
// Protetto da mutex (a differenza degli altri campi scalari di questo
// file) perche' include un campo stringa, non scrivibile/leggibile in modo
// atomico.
typedef struct {
    bool connected;
    int64_t connected_since_us;  // 0 se non connesso
    int64_t last_disconnect_us;  // 0 se mai disconnesso dopo un avvio pulito
    uint32_t connect_count;      // quante volte si e' connesso con successo dal boot (utile per notare instabilita')
    char last_error[80];         // motivo dell'ultimo errore/disconnessione, stringa vuota se nessuno finora
} ntrip_conn_status_t;

// Da chiamare subito dopo un handshake NTRIP riuscito.
void status_ntrip_note_connected(void);

// Da chiamare quando la connessione al caster cade o fallisce, con una
// breve descrizione del motivo (troncata se piu' lunga del campo
// last_error).
void status_ntrip_note_disconnected(const char *reason);

ntrip_conn_status_t status_ntrip_get(void);

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
