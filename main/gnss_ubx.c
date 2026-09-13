#include "gnss_ubx.h"

#include <string.h>
#include "esp_log.h"

static const char *TAG = "gnss_ubx";

#define UBX_SYNC1 0xB5
#define UBX_SYNC2 0x62

static void ubx_checksum(const uint8_t *data, size_t len, uint8_t *ck_a, uint8_t *ck_b)
{
    *ck_a = 0;
    *ck_b = 0;
    for (size_t i = 0; i < len; i++) {
        *ck_a = (uint8_t) (*ck_a + data[i]);
        *ck_b = (uint8_t) (*ck_b + *ck_a);
    }
}

static esp_err_t ubx_send(uart_port_t uart_num, uint8_t msg_class, uint8_t msg_id,
                           const uint8_t *payload, uint16_t payload_len)
{
    if (payload_len > 512) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t header[6] = {
        UBX_SYNC1, UBX_SYNC2, msg_class, msg_id,
        (uint8_t) (payload_len & 0xFF), (uint8_t) (payload_len >> 8),
    };

    // Il checksum UBX copre class, id, length e payload (tutto tranne i
    // due byte di sincronismo iniziali).
    uint8_t ck_buf[4 + 512];
    memcpy(ck_buf, &header[2], 4);
    if (payload_len > 0) {
        memcpy(ck_buf + 4, payload, payload_len);
    }

    uint8_t ck_a, ck_b;
    ubx_checksum(ck_buf, 4 + payload_len, &ck_a, &ck_b);

    uart_write_bytes(uart_num, (const char *) header, sizeof(header));
    if (payload_len > 0) {
        uart_write_bytes(uart_num, (const char *) payload, payload_len);
    }
    uint8_t cksum[2] = { ck_a, ck_b };
    uart_write_bytes(uart_num, (const char *) cksum, sizeof(cksum));
    return ESP_OK;
}

// Coppia chiave/valore UBX-CFG-VALSET per chiavi a 32 bit (i tipi piu'
// comuni per queste impostazioni sono E1/U4, entrambi a 4 byte).
typedef struct __attribute__((packed)) {
    uint32_t key;
    uint32_t value;
} ubx_cfg_kv32_t;

static esp_err_t ubx_valset(uart_port_t uart_num, const ubx_cfg_kv32_t *kvs, size_t n)
{
    if (n > 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t payload[4 + 32 * sizeof(ubx_cfg_kv32_t)];
    payload[0] = 0x00; // version
    payload[1] = 0x01; // layer: RAM (applicazione immediata, non persistente su BBR/flash)
    payload[2] = 0x00; // reserved
    payload[3] = 0x00; // reserved
    memcpy(payload + 4, kvs, n * sizeof(ubx_cfg_kv32_t));

    // UBX-CFG-VALSET = classe 0x06, id 0x8A
    return ubx_send(uart_num, 0x06, 0x8A, payload, (uint16_t) (4 + n * sizeof(ubx_cfg_kv32_t)));
}

esp_err_t gnss_ubx_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore u-blox come base RTK (Survey-In + RTCM3 su UART1)");

    const ubx_cfg_kv32_t kvs[] = {
        { 0x10740004, 1 },     // CFG-UART1OUTPROT-RTCM3X: abilita RTCM3 in uscita su UART1
        { 0x10740002, 0 },     // CFG-UART1OUTPROT-NMEA: disabilita NMEA in uscita (solo RTCM3 su questa porta)
        { 0x209102bd, 1 },     // CFG-MSGOUT-RTCM_3X_TYPE1005_UART1 (posizione base)
        { 0x209102cc, 1 },     // CFG-MSGOUT-RTCM_3X_TYPE1077_UART1 (GPS MSM7)
        { 0x209102d1, 1 },     // CFG-MSGOUT-RTCM_3X_TYPE1087_UART1 (GLONASS MSM7)
        { 0x2091031b, 1 },     // CFG-MSGOUT-RTCM_3X_TYPE1097_UART1 (Galileo MSM7)
        { 0x209102d6, 1 },     // CFG-MSGOUT-RTCM_3X_TYPE1127_UART1 (BeiDou MSM7)
        { 0x20910303, 1 },     // CFG-MSGOUT-RTCM_3X_TYPE1230_UART1 (bias GLONASS)
        { 0x20030001, 1 },     // CFG-TMODE-MODE = 1 (Survey-In)
        { 0x40030011, 60 },    // CFG-TMODE-SVIN-MIN-DUR: durata minima survey-in (s)
        { 0x40030010, 2500 },  // CFG-TMODE-SVIN-ACC-LIMIT: precisione richiesta, unita' 0.1mm (2500 = 250mm)
    };

    esp_err_t err = ubx_valset(uart_num, kvs, sizeof(kvs) / sizeof(kvs[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invio configurazione UBX fallito");
    }
    return err;
}

esp_err_t gnss_ubx_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore u-blox come rover (riceve RTCM3, emette NMEA/GGA su UART1)");

    const ubx_cfg_kv32_t kvs[] = {
        { 0x20030001, 0 },    // CFG-TMODE-MODE = 0 (disabilitato: non e' una base fissa)
        { 0x10730004, 1 },    // CFG-UART1INPROT-RTCM3X: accetta RTCM3 in ingresso su UART1
        { 0x10740004, 0 },    // CFG-UART1OUTPROT-RTCM3X: non serve piu' emettere RTCM3
        { 0x10740002, 1 },    // CFG-UART1OUTPROT-NMEA: riabilita NMEA in uscita (per il $GxGGA)
    };

    esp_err_t err = ubx_valset(uart_num, kvs, sizeof(kvs) / sizeof(kvs[0]));
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Invio configurazione UBX fallito");
    }
    return err;
}
