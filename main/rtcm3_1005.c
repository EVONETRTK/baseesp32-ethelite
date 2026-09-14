#include "rtcm3_1005.h"

#include <string.h>

// Bufferizza solo l'header(3) + payload dei frame che potrebbero
// interessarci + crc(3) - i frame di altro tipo (osservazioni MSM4/MSM7,
// effemeridi, spesso ben piu' lunghi) vengono scartati contando i byte
// restanti senza copiarli, cosi' non serve un buffer grande quanto il
// frame RTCM3 piu' lungo possibile (fino a ~1KB).
// Payload piu' lungo tra 1005 (19 byte) e 1006 (21 byte, con altezza
// antenna) + margine.
#define MAX_INTERESTING_PAYLOAD 24
#define FRAME_BUF_CAP (3 + MAX_INTERESTING_PAYLOAD + 3)

typedef enum {
    ST_SYNC,    // in cerca del preambolo 0xD3
    ST_LEN1,    // primo byte di lunghezza (6 bit riservati + 2 bit alti)
    ST_LEN2,    // secondo byte di lunghezza (8 bit bassi)
    ST_TYPE1,   // primo byte del payload (8 bit alti del message type a 12 bit)
    ST_TYPE2,   // secondo byte del payload (4 bit bassi del type + altro)
    ST_PAYLOAD, // resto del payload, solo se il tipo interessa
    ST_CRC,     // 3 byte di CRC, solo se il tipo interessa
    ST_SKIP,    // scarta i byte restanti di un frame che non interessa
} rtcm_state_t;

static rtcm_state_t s_state;
static uint8_t s_frame_buf[FRAME_BUF_CAP];
static size_t s_frame_pos;
static uint16_t s_payload_len;
static uint16_t s_msg_type;
static size_t s_skip_remaining;

void rtcm3_1005_init(void)
{
    s_state = ST_SYNC;
    s_frame_pos = 0;
}

// CRC24Q (polinomio 0x1864CFB, non riflesso, valore iniziale 0) come da
// standard RTCM 10403.x - versione bit-a-bit (senza tabella) perche' il
// volume di dati da un singolo ricevitore base e' basso, non serve
// ottimizzare la velocita' a scapito della memoria flash di una tabella.
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

// Estrae un intero con segno a 38 bit da buf, a partire dal bit
// bit_offset (0 = MSB del primo byte di buf), come da codifica RTCM3
// (MSB-first, complemento a due).
static int64_t extract_bits_i38(const uint8_t *buf, size_t bit_offset)
{
    uint64_t raw = 0;
    for (int i = 0; i < 38; i++) {
        size_t bit_pos = bit_offset + (size_t) i;
        uint8_t byte = buf[bit_pos / 8];
        int bit_in_byte = 7 - (int) (bit_pos % 8);
        uint8_t bit = (byte >> bit_in_byte) & 1;
        raw = (raw << 1) | bit;
    }
    if (raw & (1ULL << 37)) {
        raw |= ~((1ULL << 38) - 1); // sign-extend da 38 a 64 bit
    }
    return (int64_t) raw;
}

// s_frame_buf contiene a questo punto: header(3) + payload(s_payload_len)
// + crc(3), tutti gia' ricevuti. Verifica il CRC24Q (calcolato su
// header+payload, esclusi i 3 byte di crc) e, se valido, estrae la
// posizione ECEF - stesso bit layout per 1005 e 1006, l'altezza antenna
// aggiuntiva di 1006 (in coda al payload) viene semplicemente ignorata.
static bool try_decode_frame(rtcm3_position_t *out_pos)
{
    size_t header_and_payload_len = 3 + s_payload_len;
    uint32_t crc = 0;
    for (size_t i = 0; i < header_and_payload_len; i++) {
        crc = crc24q_update(crc, s_frame_buf[i]);
    }
    uint32_t received_crc = ((uint32_t) s_frame_buf[header_and_payload_len] << 16) |
                             ((uint32_t) s_frame_buf[header_and_payload_len + 1] << 8) |
                             ((uint32_t) s_frame_buf[header_and_payload_len + 2]);
    if (crc != received_crc) {
        return false;
    }

    // Layout campi del payload (bit, da inizio payload): Message Number
    // (12), Reference Station ID (12), ITRF Realization Year (6), GPS/
    // GLONASS/Galileo indicator (1+1+1), Reference-Station Indicator (1),
    // ECEF-X (38) a partire dal bit 34, Single Receiver Oscillator
    // Indicator (1), Reserved (1), ECEF-Y (38) dal bit 74, Quarter Cycle
    // Indicator (2), ECEF-Z (38) dal bit 114. Risoluzione 0,0001 m.
    const uint8_t *payload = &s_frame_buf[3];
    int64_t x_raw = extract_bits_i38(payload, 34);
    int64_t y_raw = extract_bits_i38(payload, 74);
    int64_t z_raw = extract_bits_i38(payload, 114);

    out_pos->ecef_x_m = (double) x_raw * 0.0001;
    out_pos->ecef_y_m = (double) y_raw * 0.0001;
    out_pos->ecef_z_m = (double) z_raw * 0.0001;
    return true;
}

bool rtcm3_1005_feed(const uint8_t *data, size_t len, rtcm3_position_t *out_pos)
{
    bool found = false;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        switch (s_state) {
        case ST_SYNC:
            if (b == 0xD3) {
                s_frame_buf[0] = b;
                s_frame_pos = 1;
                s_state = ST_LEN1;
            }
            break;

        case ST_LEN1:
            s_frame_buf[1] = b;
            s_frame_pos = 2;
            s_state = ST_LEN2;
            break;

        case ST_LEN2:
            s_frame_buf[2] = b;
            s_frame_pos = 3;
            s_payload_len = (((uint16_t) s_frame_buf[1] & 0x03) << 8) | s_frame_buf[2];
            if (s_payload_len < 2) {
                // Troppo corto per contenere il campo tipo messaggio (12
                // bit): non puo' essere 1005/1006, torna subito in sync.
                s_state = ST_SYNC;
            } else {
                s_state = ST_TYPE1;
            }
            break;

        case ST_TYPE1:
            s_frame_buf[s_frame_pos++] = b;
            s_state = ST_TYPE2;
            break;

        case ST_TYPE2: {
            s_frame_buf[s_frame_pos++] = b;
            s_msg_type = ((uint16_t) s_frame_buf[3] << 4) | (s_frame_buf[4] >> 4);
            size_t remaining_payload = (size_t) s_payload_len - 2; // 2 byte di payload gia' letti sopra
            bool interesting = (s_msg_type == 1005 || s_msg_type == 1006) &&
                                s_payload_len <= MAX_INTERESTING_PAYLOAD;
            if (interesting) {
                s_state = ST_PAYLOAD;
            } else {
                s_skip_remaining = remaining_payload + 3; // + crc
                s_state = ST_SKIP;
            }
            break;
        }

        case ST_PAYLOAD:
            s_frame_buf[s_frame_pos++] = b;
            if (s_frame_pos >= 3 + (size_t) s_payload_len) {
                s_skip_remaining = 3; // ora arrivano i 3 byte di crc
                s_state = ST_CRC;
            }
            break;

        case ST_CRC:
            s_frame_buf[s_frame_pos++] = b;
            s_skip_remaining--;
            if (s_skip_remaining == 0) {
                if (try_decode_frame(out_pos)) {
                    found = true;
                }
                s_state = ST_SYNC;
            }
            break;

        case ST_SKIP:
            s_skip_remaining--;
            if (s_skip_remaining == 0) {
                s_state = ST_SYNC;
            }
            break;
        }
    }

    return found;
}
