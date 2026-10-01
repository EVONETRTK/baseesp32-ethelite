#pragma once

#include <sys/socket.h>
#include <sys/select.h>
#include <unistd.h>
#include <fcntl.h>
#include <errno.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "status.h"
#include "eth_link.h"

// Spiegazione leggibile degli errori di connessione piu' comuni, per il
// pannello e i log (invece di "errno 116"). NULL se non e' tra questi.
static inline const char *net_errno_text(int e)
{
    switch (e) {
    case ETIMEDOUT:    return "il caster non risponde (nessuna risposta entro il tempo limite)";
    case ECONNREFUSED: return "connessione rifiutata: porta sbagliata o caster spento";
    case EHOSTUNREACH:
    case ENETUNREACH:  return "caster non raggiungibile: la scheda non ha accesso alla rete";
    case ECONNRESET:   return "connessione interrotta dal caster";
    default:           return NULL;
    }
}

// connect() con un limite di tempo vero. SO_SNDTIMEO/SO_RCVTIMEO non valgono
// per connect(): verso un indirizzo che non risponde restava bloccata ~18 s
// (misurato), e il pulsante "Prova connessione" teneva fermo il pannello per
// tutto quel tempo. Ritorna 0 se collegato, -1 con errno impostato
// (ETIMEDOUT allo scadere).
static inline int net_connect_timeout(int sock, const struct sockaddr *addr, socklen_t len, int timeout_s)
{
    int flags = fcntl(sock, F_GETFL, 0);
    fcntl(sock, F_SETFL, flags | O_NONBLOCK);
    int rc = connect(sock, addr, len);
    if (rc != 0 && errno == EINPROGRESS) {
        fd_set wfds;
        FD_ZERO(&wfds);
        FD_SET(sock, &wfds);
        struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
        rc = select(sock + 1, NULL, &wfds, NULL, &tv);
        if (rc == 1) {
            int so_err = 0;
            socklen_t so_len = sizeof(so_err);
            getsockopt(sock, SOL_SOCKET, SO_ERROR, &so_err, &so_len);
            rc = so_err ? -1 : 0;
            if (so_err) {
                errno = so_err;
            }
        } else {
            if (rc == 0) {
                errno = ETIMEDOUT;
            }
            rc = -1;
        }
    }
    fcntl(sock, F_SETFL, flags);
    return rc;
}

// Chiude subito un socket TCP client con un RST (SO_LINGER a 0), senza
// lasciarlo nello stato TIME_WAIT per 2*MSL (120 s con CONFIG_LWIP_TCP_MSL
// a 60000). I client NTRIP ritentano ogni pochi secondi quando il caster
// rifiuta: con la chiusura normale i PCB in TIME_WAIT si accumulavano oltre
// CONFIG_LWIP_MAX_ACTIVE_TCP (16) e dopo qualche minuto la scheda non
// riusciva piu' a rispondere via TCP - pannello web bloccato anche se il
// ping passava (visto sul dispositivo). Richiede CONFIG_LWIP_SO_LINGER=y.
static inline void net_close_now(int sock)
{
    struct linger lin = { .l_onoff = 1, .l_linger = 0 };
    setsockopt(sock, SOL_SOCKET, SO_LINGER, &lin, sizeof(lin));
    close(sock);
}

// Aspetta che ci sia una rete attiva (WiFi o cellulare con IP, oppure
// Ethernet), al massimo timeout_ms. Usata dai client NTRIP prima di ogni
// tentativo: all'avvio il primo partiva prima del WiFi e falliva sempre con
// "Indirizzo del caster non trovato (DNS)", poi 5 s di attesa inutile.
// Scaduto il tempo si prova comunque, cosi' l'errore vero arriva al pannello.
static inline bool net_wait_ready(uint32_t timeout_ms)
{
    for (uint32_t waited = 0; ; waited += 250) {
        if (status_get_net() != NET_STATUS_NONE || eth_link_is_connected()) {
            return true;
        }
        if (waited >= timeout_ms) {
            return false;
        }
        vTaskDelay(pdMS_TO_TICKS(250));
    }
}
