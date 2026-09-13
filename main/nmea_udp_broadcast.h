#pragma once

#include <stddef.h>

// Apre il socket UDP di broadcast. Va chiamata una volta all'avvio, dopo
// che le interfacce di rete sono state inizializzate.
void nmea_udp_broadcast_init(void);

// Invia una riga NMEA (senza CR/LF) in broadcast UDP sulla porta
// configurata (settings.nmea_udp_port), su tutte le interfacce attive
// (AP di setup, WiFi station, Ethernet) - utile per software come
// AgOpenGPS/AgIO che ricevono la posizione GPS via rete locale.
void nmea_udp_broadcast_send(const char *line, size_t len);
