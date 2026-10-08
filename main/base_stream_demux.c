#include "base_stream_demux.h"
#include "gnss_fix.h"
#include "gnss_signal.h"
#include "gnss_unicore.h"
#include "gnss_comnav.h"

#include <string.h>

// RTCM3: preambolo 0xD3, 6 bit riservati a zero + 10 bit di lunghezza del
// payload, payload, CRC24Q (3 byte). Standard RTCM 10403.x.
#define RTCM_PREAMBLE       0xD3
#define RTCM_MAX_PAYLOAD    1023
#define RTCM_MAX_FRAME      (3 + RTCM_MAX_PAYLOAD + 3)
#define NMEA_MAX_LINE       256 // anche le righe ASCII Unicore con '#' (#VERSIONA ~200 caratteri)

typedef enum {
    ST_IDLE = 0,
    ST_RTCM,
    ST_NMEA,
    ST_UBX_SYNC2,
    ST_UBX_BODY,
} demux_state_t;

static demux_state_t s_state = ST_IDLE;

static uint8_t s_rtcm[RTCM_MAX_FRAME];
static size_t s_rtcm_pos;
static size_t s_rtcm_total;

static char s_nmea[NMEA_MAX_LINE];
static size_t s_nmea_pos;

// UBX: 0xB5 0x62, classe, id, lunghezza (2 byte LE), payload, checksum (2).
// Si conta solo quanti byte saltare: il contenuto lo legge gia'
// gnss_ubx_ack.c.
static uint8_t s_ubx_hdr[4];
static size_t s_ubx_pos;
static size_t s_ubx_remaining;

// Stesso CRC24Q di rtcm3_1005.c (polinomio 0x1864CFB, valore iniziale 0).
static uint32_t crc24q_update(uint32_t crc, uint8_t byte)
{
    crc ^= ((uint32_t) byte) << 16;
    for (int i = 0; i < 8; i++) {
        crc <<= 1;
        if (crc & 0x01000000) {
            crc ^= 0x01864CFB;
        }
    }
    return crc & 0x00FFFFFF;
}

static void rtcm_frame_done(base_demux_rtcm_cb_t on_rtcm)
{
    size_t body = s_rtcm_total - 3;
    uint32_t crc = 0;
    for (size_t i = 0; i < body; i++) {
        crc = crc24q_update(crc, s_rtcm[i]);
    }
    uint32_t got = ((uint32_t) s_rtcm[body] << 16) | ((uint32_t) s_rtcm[body + 1] << 8) | s_rtcm[body + 2];
    if (crc == got && on_rtcm) {
        on_rtcm(s_rtcm, s_rtcm_total);
    }
}

static void nmea_line_done(void)
{
    s_nmea[s_nmea_pos] = '\0';
    if (s_nmea[0] == '#') {
        gnss_unicore_note_line(s_nmea);
        gnss_comnav_note_line(s_nmea);
        return;
    }
    // '$' + talker (2 caratteri) + tipo (3), es. "$GNGGA", "$GPGSV".
    if (s_nmea_pos > 6) {
        if (memcmp(&s_nmea[3], "GGA", 3) == 0) {
            gnss_fix_parse_gga(s_nmea);
        } else if (memcmp(&s_nmea[3], "GSV", 3) == 0) {
            gnss_signal_parse_gsv(s_nmea);
        }
    }
}

void base_stream_demux_feed(const uint8_t *buf, size_t len, base_demux_rtcm_cb_t on_rtcm)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        switch (s_state) {
        case ST_IDLE:
            if (c == RTCM_PREAMBLE) {
                s_rtcm[0] = c;
                s_rtcm_pos = 1;
                s_rtcm_total = 0;
                s_state = ST_RTCM;
            } else if (c == '$' || c == '#') {
                s_nmea[0] = (char) c;
                s_nmea_pos = 1;
                s_state = ST_NMEA;
            } else if (c == 0xB5) {
                s_state = ST_UBX_SYNC2;
            }
            // Qualunque altra cosa (fine riga, spazzatura) si ignora.
            break;

        case ST_RTCM:
            s_rtcm[s_rtcm_pos++] = c;
            if (s_rtcm_pos == 3) {
                // I 6 bit alti dopo il preambolo sono riservati a zero: se
                // non lo sono, 0xD3 era un byte qualunque, non un frame.
                if (s_rtcm[1] & 0xFC) {
                    s_state = ST_IDLE;
                    break;
                }
                s_rtcm_total = 3 + (((size_t) (s_rtcm[1] & 0x03) << 8) | s_rtcm[2]) + 3;
            }
            if (s_rtcm_total && s_rtcm_pos == s_rtcm_total) {
                rtcm_frame_done(on_rtcm);
                s_state = ST_IDLE;
            }
            break;

        case ST_NMEA:
            if (c == '\n' || c == '\r') {
                nmea_line_done();
                s_state = ST_IDLE;
            } else if (c < 0x20 || c > 0x7E || s_nmea_pos >= NMEA_MAX_LINE - 1) {
                // Non ASCII stampabile o riga troppo lunga: non era NMEA.
                s_state = ST_IDLE;
            } else {
                s_nmea[s_nmea_pos++] = (char) c;
            }
            break;

        case ST_UBX_SYNC2:
            if (c == 0x62) {
                s_ubx_pos = 0;
                s_state = ST_UBX_BODY;
                s_ubx_remaining = 0;
            } else {
                s_state = ST_IDLE;
            }
            break;

        case ST_UBX_BODY:
            if (s_ubx_pos < sizeof(s_ubx_hdr)) {
                s_ubx_hdr[s_ubx_pos++] = c;
                if (s_ubx_pos == sizeof(s_ubx_hdr)) {
                    s_ubx_remaining = (size_t) (s_ubx_hdr[2] | (s_ubx_hdr[3] << 8)) + 2;
                    // 8192: i frame RXM-RAWX (registrazione dei dati grezzi) superano
                    // i 2,5 KB con 4 costellazioni. Con il limite a 2048 il frame
                    // veniva abbandonato a meta' e i suoi byte riletti come possibili
                    // inizi di RTCM, con il rischio di perdere il frame RTCM seguente.
                    if (s_ubx_remaining > 8192) {
                        // Lunghezza assurda per le risposte che arrivano qui:
                        // non era un frame UBX, meglio non saltare dati.
                        s_state = ST_IDLE;
                    }
                }
            } else if (--s_ubx_remaining == 0) {
                s_state = ST_IDLE;
            }
            break;
        }
    }
}
