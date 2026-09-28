#pragma once

#include <sys/socket.h>
#include <unistd.h>

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
