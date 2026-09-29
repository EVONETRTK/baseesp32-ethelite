#include "gnss_ubx_ack.h"
#include "status.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "esp_log.h"

static const char *TAG = "gnss_ubx_ack";

typedef struct {
    bool acked;
    uint8_t cls;
    uint8_t id;
} ubx_ack_result_t;

// Un solo slot: questo modulo assume un comando alla volta (invio, poi
// attesa immediata della risposta prima del comando successivo - e' cosi'
// che gnss_ubx.c usa ubx_valset()), mai due attese in sovrapposizione.
static QueueHandle_t s_ack_queue;

typedef enum {
    ST_IDLE = 0,
    ST_SYNC2,
    ST_CLASS,
    ST_ID,
    ST_LEN1,
    ST_LEN2,
    ST_PAYLOAD,
    ST_CKA,
    ST_CKB,
} ubx_frame_state_t;

static ubx_frame_state_t s_state = ST_IDLE;
static uint8_t s_class, s_id;
static uint16_t s_len, s_payload_idx;
// 220 byte: capiente per UBX-MON-VER (swVersion[30]+hwVersion[10]+fino a
// diverse righe di estensione[30] l'una) - usato solo per diagnostica
// (log del contenuto), non serve per il percorso ACK/NAK vero e proprio
// che guarda solo i primi 2 byte.
static uint8_t s_payload[220];
static uint8_t s_ck_a, s_calc_ck_a, s_calc_ck_b;

// Finestra di attesa risposta (vedi gnss_ubx_ack_arm()): scritto dal task
// che manda i comandi, letto dal task che legge la UART.
static volatile bool s_armed;

void gnss_ubx_ack_init(void)
{
    s_ack_queue = xQueueCreate(1, sizeof(ubx_ack_result_t));
    s_state = ST_IDLE;
}

void gnss_ubx_ack_arm(void)
{
    if (s_ack_queue) {
        xQueueReset(s_ack_queue); // scarta un eventuale ACK residuo di un comando precedente
    }
    s_armed = true;
}

static void ubx_frame_checksum_add(uint8_t c)
{
    s_calc_ck_a = (uint8_t) (s_calc_ck_a + c);
    s_calc_ck_b = (uint8_t) (s_calc_ck_b + s_calc_ck_a);
}

bool gnss_ubx_ack_feed_byte(uint8_t c)
{
    switch (s_state) {
    case ST_IDLE:
        if (s_armed && c == 0xB5) {
            s_state = ST_SYNC2;
            return true;
        }
        return false;

    case ST_SYNC2:
        if (c == 0x62) {
            s_state = ST_CLASS;
            s_calc_ck_a = 0;
            s_calc_ck_b = 0;
            return true;
        }
        // Falso allarme (0xB5 isolato): non era l'inizio di un frame, il
        // byte corrente non appartiene a UBX e torna al chiamante.
        s_state = ST_IDLE;
        return false;

    case ST_CLASS:
        s_class = c;
        ubx_frame_checksum_add(c);
        s_state = ST_ID;
        return true;

    case ST_ID:
        s_id = c;
        ubx_frame_checksum_add(c);
        s_state = ST_LEN1;
        return true;

    case ST_LEN1:
        s_len = c;
        ubx_frame_checksum_add(c);
        s_state = ST_LEN2;
        return true;

    case ST_LEN2:
        s_len |= ((uint16_t) c) << 8;
        ubx_frame_checksum_add(c);
        if (s_len > sizeof(s_payload)) {
            // Nessuna risposta attesa qui (ACK/NAK, MON-VER) e' cosi' lunga:
            // quasi certamente un 0xB5 0x62 casuale in mezzo ad altri dati -
            // meglio lasciar perdere che inghiottire fino a 64KB di stream.
            s_state = ST_IDLE;
            return true;
        }
        s_payload_idx = 0;
        s_state = (s_len == 0) ? ST_CKA : ST_PAYLOAD;
        return true;

    case ST_PAYLOAD:
        if (s_payload_idx < sizeof(s_payload)) {
            s_payload[s_payload_idx] = c;
        }
        ubx_frame_checksum_add(c);
        s_payload_idx++;
        if (s_payload_idx >= s_len) {
            s_state = ST_CKA;
        }
        return true;

    case ST_CKA:
        // Non serve confrontare qui: se sbagliato, ck_b sotto non potra'
        // mai combaciare con s_calc_ck_b (dipende da entrambi i byte del
        // messaggio originale) - un solo controllo finale basta.
        s_ck_a = c;
        s_state = ST_CKB;
        return true;

    case ST_CKB: {
        uint8_t ck_b = c;
        bool checksum_ok = (s_ck_a == s_calc_ck_a && ck_b == s_calc_ck_b);
        if (checksum_ok && s_class == 0x05 && (s_id == 0x01 || s_id == 0x00) && s_len == 2) {
            ubx_ack_result_t res = {
                .acked = (s_id == 0x01),
                .cls = s_payload[0],
                .id = s_payload[1],
            };
            if (s_ack_queue) {
                xQueueOverwrite(s_ack_queue, &res); // solo l'ultimo interessa
            }
        } else if (checksum_ok && s_class == 0x01 && s_id == 0x3B && s_len >= 40) {
            // UBX-NAV-SVIN (u-blox Interface Description): dur U4 @8 (s),
            // meanAcc U4 @28 (0,1 mm), obs U4 @32, valid U1 @36, active U1 @37.
            uint32_t dur = (uint32_t) s_payload[8] | ((uint32_t) s_payload[9] << 8) |
                           ((uint32_t) s_payload[10] << 16) | ((uint32_t) s_payload[11] << 24);
            uint32_t acc = (uint32_t) s_payload[28] | ((uint32_t) s_payload[29] << 8) |
                           ((uint32_t) s_payload[30] << 16) | ((uint32_t) s_payload[31] << 24);
            uint32_t obs = (uint32_t) s_payload[32] | ((uint32_t) s_payload[33] << 8) |
                           ((uint32_t) s_payload[34] << 16) | ((uint32_t) s_payload[35] << 24);
            status_svin_note(s_payload[37] != 0, s_payload[36] != 0, dur, acc / 10000.0f, obs);
        } else if (checksum_ok && s_class == 0x0A && s_id == 0x04) {
            // UBX-MON-VER: risposta a un poll diagnostico (non un ACK/NAK),
            // usata per sapere con certezza cosa risponde davvero questo
            // modulo (versione SW/HW/protocollo) - payload = swVersion
            // (stringa C a 30 byte) + hwVersion (10 byte) + righe extension
            // opzionali (30 byte l'una, tipicamente include "PROTVER=x.xx").
            char sw[31] = {0}, hw[11] = {0};
            size_t sw_len = s_payload_idx < 30 ? s_payload_idx : 30;
            memcpy(sw, s_payload, sw_len);
            if (s_payload_idx > 30) {
                size_t hw_len = (s_payload_idx - 30) < 10 ? (s_payload_idx - 30) : 10;
                memcpy(hw, s_payload + 30, hw_len);
            }
            ESP_LOGI(TAG, "UBX-MON-VER: swVersion=\"%s\" hwVersion=\"%s\"", sw, hw);
            for (size_t off = 40; off + 30 <= s_payload_idx && off + 30 <= sizeof(s_payload); off += 30) {
                char ext[31] = {0};
                memcpy(ext, s_payload + off, 30);
                ESP_LOGI(TAG, "UBX-MON-VER extension: \"%s\"", ext);
            }
        } else if (!checksum_ok) {
            ESP_LOGD(TAG, "Frame UBX con checksum non valido scartato (classe 0x%02X id 0x%02X)", s_class, s_id);
        }
        s_state = ST_IDLE;
        return true;
    }
    }
    return false;
}

bool gnss_ubx_ack_wait(uint32_t timeout_ms, bool *out_acked, uint8_t *out_echo_cls, uint8_t *out_echo_id)
{
    if (!s_ack_queue) {
        return false;
    }

    // Niente xQueueReset() qui: il comando e' gia' partito e un ACK veloce
    // poteva arrivare prima di questa chiamata ed essere scartato - lo
    // svuotamento avviene in gnss_ubx_ack_arm(), prima dell'invio.
    ubx_ack_result_t res;
    bool got = xQueueReceive(s_ack_queue, &res, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
    s_armed = false;
    if (!got) {
        return false;
    }
    if (out_acked) {
        *out_acked = res.acked;
    }
    if (out_echo_cls) {
        *out_echo_cls = res.cls;
    }
    if (out_echo_id) {
        *out_echo_id = res.id;
    }
    return true;
}
