#include "gnss_nmea_reader.h"
#include "sys_stats.h"
#include "gnss_io.h"
#include "nmea_udp_broadcast.h"
#include "gnss_signal.h"
#include "gnss_fix.h"
#include "gnss_ubx_ack.h"
#include "ntrip_rover_client.h"
#include "status.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define READ_BUF_SIZE 256
// 128 bastava per le sole righe NMEA; le righe ASCII Bynav #INSPVAXA (lat/
// lon/quota/velocita'/assetto/deviazioni standard, ~20 campi) sono molto
// piu' lunghe - alzato per non troncarle (vedi handle_bynav_line() sotto).
#define LINE_BUF_SIZE 320

// Estrae il campo N (0-based) della sezione dati di una riga ASCII in stile
// Bynav/NovAtel OEM7 (nomi di log come #INSPVAXA/#HEADINGA riusati
// identici da Bynav): intestazione e dati separati dal primo ';', campi
// dati separati da ',', fino all'eventuale checksum "*xx" finale. Ritorna
// false se il campo richiesto non esiste (riga troncata/malformata) -
// meglio scartare il valore che usarne uno spostato di campo.
static bool bynav_data_field(const char *line, int field_index, char *out, size_t out_size)
{
    const char *data = strchr(line, ';');
    if (!data) {
        return false;
    }
    data++;
    for (int i = 0; i < field_index; i++) {
        data = strchr(data, ',');
        if (!data) {
            return false;
        }
        data++;
    }
    const char *end = data;
    while (*end && *end != ',' && *end != '*') {
        end++;
    }
    size_t len = (size_t)(end - data);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    return true;
}

// Layout campi dati basato sul formato NovAtel OEM7 (Bynav riusa gli stessi
// nomi di log #INSPVAXA/#HEADINGA e la stessa convenzione intestazione/dati
// - vedi AN065_QuickStart.pdf) - MAI verificato contro l'output reale di un
// modulo Bynav M21D (nessun hardware disponibile in questo progetto). Da
// controllare via log seriale reale prima di fare affidamento su questi
// valori per qualunque uso in campo (es. autosterzo).
//
// #INSPVAXA campi dati: 0=ins_status, 1=pos_type, 2=lat, 3=lon, 4=height,
// 5=undulation, 6=north_vel, 7=east_vel, 8=up_vel, 9=roll, 10=pitch,
// 11=azimuth(heading), poi le deviazioni standard corrispondenti (non lette qui).
static void handle_inspvaxa(const char *line)
{
    char status[24], roll[16], pitch[16], azimuth[16];
    if (!bynav_data_field(line, 0, status, sizeof(status))) return;
    if (!bynav_data_field(line, 9, roll, sizeof(roll))) return;
    if (!bynav_data_field(line, 10, pitch, sizeof(pitch))) return;
    if (!bynav_data_field(line, 11, azimuth, sizeof(azimuth))) return;
    status_bynav_ins_note_inspvaxa(status, (float) atof(roll), (float) atof(pitch), (float) atof(azimuth));
}

// #HEADINGA campi dati: 0=sol_status, 1=pos_type, 2=length, 3=heading,
// 4=pitch, poi deviazioni standard/altri campi (non letti qui). Nessuno
// stato di allineamento INS in questo log (solo in INSPVAXA sopra).
static void handle_headinga(const char *line)
{
    char heading[16], pitch[16];
    if (!bynav_data_field(line, 3, heading, sizeof(heading))) return;
    if (!bynav_data_field(line, 4, pitch, sizeof(pitch))) return;
    status_bynav_ins_note_headinga((float) atof(heading), (float) atof(pitch));
}

// Righe ASCII Bynav (prefisso '#', non NMEA) - solo i due log di prua/
// assetto INS ci interessano qui (GNSS_CHIP_BYNAV_M21D rover, vedi
// gnss_bynav_m21d_configure_rover()); BESTPOSA/INSCALSTATUSA arrivano sulla
// stessa UART ma non sono ancora parsati (vedi nota in status.h).
static void handle_bynav_line(const char *line)
{
    if (strncmp(line, "#INSPVAXA", 9) == 0) {
        handle_inspvaxa(line);
    } else if (strncmp(line, "#HEADINGA", 9) == 0) {
        handle_headinga(line);
    }
}

void gnss_nmea_reader_task(void *arg)
{
    (void) arg; // porta verso il ricevitore gestita da gnss_io (seriale o I2C)
    uint8_t read_buf[READ_BUF_SIZE];
    char line[LINE_BUF_SIZE];
    size_t line_len = 0;

    while (1) {
        sys_stats_heartbeat(HB_GNSS);
        int n = gnss_io_read(read_buf, sizeof(read_buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char) read_buf[i];

            // Frame binari UBX-ACK-ACK/NAK (risposta ai comandi di
            // configurazione mandati da gnss_ubx.c) possono arrivare
            // interposti alle righe NMEA sulla stessa UART - la firma
            // 0xB5 0x62 e' inequivocabile (mai presente in NMEA, puro
            // ASCII stampabile), quindi si possono distinguere senza
            // ambiguita' prima di provare a trattarli come NMEA.
            if (gnss_ubx_ack_feed_byte((uint8_t) c)) {
                continue;
            }

            if (c == '\n') {
                if (line_len > 6 && line[0] == '$') {
                    line[line_len] = '\0';

                    nmea_udp_broadcast_send(line, line_len);

                    // Formato NMEA: '$' + talker (2 char) + tipo sentenza
                    // (3 char), es. "$GPGGA"/"$GNGGA"/"$GPGSV"...
                    if (memcmp(&line[3], "GGA", 3) == 0) {
                        ntrip_rover_client_forward_gga(line, line_len);
                        gnss_fix_parse_gga(line);
                    } else if (memcmp(&line[3], "GSV", 3) == 0) {
                        gnss_signal_parse_gsv(line);
                    }
                } else if (line_len > 10 && line[0] == '#') {
                    line[line_len] = '\0';
                    handle_bynav_line(line);
                }
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < LINE_BUF_SIZE - 1) {
                    line[line_len++] = (char) c;
                } else {
                    // Riga troppo lunga (improbabile per NMEA standard):
                    // scarta e riparte dalla prossima, per non restare
                    // bloccati a concatenare spazzatura all'infinito.
                    line_len = 0;
                }
            }
        }
    }
}
