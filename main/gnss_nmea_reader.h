#pragma once

// Task attivo in modalita' rover: legge lo stream NMEA emesso dal GNSS
// sulla UART indicata, lo ricompone in righe complete (bufferizzando tra
// una lettura e l'altra, a differenza della vecchia extract_gga() che
// assumeva una riga per lettura), e lo smista a tre destinazioni:
//  - broadcast UDP su WiFi/Ethernet (nmea_udp_broadcast), per software
//    come AgOpenGPS/AgIO;
//  - inoltro delle righe $GxGGA al client NTRIP rover
//    (ntrip_rover_client_forward_gga), per rimandare la posizione al
//    caster;
//  - aggiornamento dello stato satelliti dalle righe $GxGSV
//    (gnss_signal_parse_gsv), per il grafico segnali nella UI.
// arg = uart_port_t incapsulato come (void *)(intptr_t) uart_num.
void gnss_nmea_reader_task(void *arg);
