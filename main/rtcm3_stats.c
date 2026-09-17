#include "rtcm3_stats.h"

#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Fino a 24 tipi di messaggio distinti tracciati contemporaneamente - ben
// piu' dei 14 selezionabili dalla UI (vedi settings.h, rtcm_*_enable) piu'
// qualche margine per messaggi non nella lista (es. 1033 su Unicore). Un
// tipo mai visto prima quando la tabella e' gia' piena viene ignorato
// silenziosamente: accettabile per un contatore diagnostico.
#define MAX_TRACKED_TYPES 24

static SemaphoreHandle_t s_mutex;
static rtcm3_stat_entry_t s_table[MAX_TRACKED_TYPES];
static size_t s_table_len;

typedef enum {
    ST_SYNC,  // in cerca del preambolo 0xD3
    ST_LEN1,  // primo byte di lunghezza (6 bit riservati + 2 bit alti)
    ST_LEN2,  // secondo byte di lunghezza (8 bit bassi)
    ST_TYPE1, // primo byte del payload (8 bit alti del message type a 12 bit)
    ST_TYPE2, // secondo byte del payload (4 bit bassi del type + altro)
    ST_REST,  // resto del frame (payload + crc), solo conteggio byte
} rtcm_state_t;

static rtcm_state_t s_state;
static uint8_t s_len1_byte;
static uint8_t s_type1_byte;
static uint16_t s_payload_len;
static uint16_t s_msg_type;
static size_t s_frame_total_len; // 3 (header) + payload_len + 3 (crc)
static size_t s_bytes_in_frame;

// Il mutex si crea SOLO qui, non "pigramente" al primo uso in ciascuna
// funzione (come altrove in questo progetto): rtcm3_stats_init() e' l'unica
// chiamata sincrona in app_main() (vedi main.c), garantita finire prima
// che qualunque task possa chiamare rtcm3_stats_feed()/_get() - creare il
// mutex pigramente in piu' punti avrebbe un margine di corsa teorico se due
// task lo usassero per la prima volta in contemporanea (due mutex diversi
// creati, uno dei due perso), evitato qui alla radice invece che accettato
// come rischio residuo.
void rtcm3_stats_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_table_len = 0;
    s_state = ST_SYNC;
}

static void add_bytes(uint16_t type, uint32_t bytes)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_table_len; i++) {
        if (s_table[i].msg_type == type) {
            s_table[i].bytes += bytes;
            xSemaphoreGive(s_mutex);
            return;
        }
    }
    if (s_table_len < MAX_TRACKED_TYPES) {
        s_table[s_table_len].msg_type = type;
        s_table[s_table_len].bytes = bytes;
        s_table_len++;
    }
    xSemaphoreGive(s_mutex);
}

void rtcm3_stats_feed(const uint8_t *data, size_t len)
{
    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        switch (s_state) {
        case ST_SYNC:
            if (b == 0xD3) {
                s_bytes_in_frame = 1;
                s_state = ST_LEN1;
            }
            break;

        case ST_LEN1:
            s_len1_byte = b;
            s_bytes_in_frame++;
            s_state = ST_LEN2;
            break;

        case ST_LEN2:
            s_payload_len = (((uint16_t) s_len1_byte & 0x03) << 8) | b;
            s_bytes_in_frame++;
            if (s_payload_len < 2) {
                // Troppo corto per contenere il campo tipo messaggio (12
                // bit): non e' un frame valido, torna subito in sync.
                s_state = ST_SYNC;
            } else {
                s_frame_total_len = 3 + (size_t) s_payload_len + 3;
                s_state = ST_TYPE1;
            }
            break;

        case ST_TYPE1:
            s_type1_byte = b;
            s_bytes_in_frame++;
            s_state = ST_TYPE2;
            break;

        case ST_TYPE2:
            s_msg_type = ((uint16_t) s_type1_byte << 4) | (b >> 4);
            s_bytes_in_frame++;
            s_state = ST_REST;
            break;

        case ST_REST:
            s_bytes_in_frame++;
            if (s_bytes_in_frame >= s_frame_total_len) {
                add_bytes(s_msg_type, (uint32_t) s_frame_total_len);
                s_state = ST_SYNC;
            }
            break;
        }
    }
}

size_t rtcm3_stats_get(rtcm3_stat_entry_t *out, size_t max_count)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t n = s_table_len < max_count ? s_table_len : max_count;
    memcpy(out, s_table, n * sizeof(rtcm3_stat_entry_t));
    xSemaphoreGive(s_mutex);
    return n;
}
