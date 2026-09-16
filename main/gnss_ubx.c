#include "gnss_ubx.h"
#include "settings.h"

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

// Chiave MSM4/MSM7 (UART1) per ciascuna costellazione, verificate contro
// sparkfun/SparkFun_u-blox_GNSS_Arduino_Library (src/u-blox_config_keys.h,
// libreria di terzi ampiamente usata, non la fonte ufficiale u-blox diretta
// ma un riscontro indipendente) dopo aver scoperto che i valori precedenti
// in questo file erano sbagliati (quasi tutti spostati di una posizione:
// es. 1005 era 0x209102bd invece di 0x209102be) - la configurazione RTCM3
// della base molto probabilmente non ha mai funzionato correttamente su
// hardware u-blox reale prima di questo fix, dato che una chiave sbagliata
// viene rifiutata in silenzio (vedi avvertenza in gnss_ubx.h).
typedef struct {
    uint32_t msm4_key;
    uint32_t msm7_key;
} ubx_msm_keys_t;

static const ubx_msm_keys_t UBX_MSM_KEYS_GPS     = { 0x2091035f, 0x209102cd };
static const ubx_msm_keys_t UBX_MSM_KEYS_GLONASS = { 0x20910364, 0x209102d2 };
static const ubx_msm_keys_t UBX_MSM_KEYS_GALILEO = { 0x20910369, 0x20910319 };
static const ubx_msm_keys_t UBX_MSM_KEYS_BEIDOU  = { 0x2091036e, 0x209102d7 };

// Aggiunge a kvs (fino a max, aggiornando *n) le due chiavi msm4/msm7 di
// una costellazione, impostando ad 1 solo quella corrispondente al livello
// richiesto e a 0 l'altra - un cambio MSM4->MSM7 (o viceversa) deve
// disattivare esplicitamente il livello precedente, non basta abilitare
// quello nuovo.
static void add_msm_level(ubx_cfg_kv32_t *kvs, size_t *n, size_t max, const ubx_msm_keys_t *keys, rtcm_msm_level_t level)
{
    if (*n + 2 > max) {
        return;
    }
    kvs[(*n)++] = (ubx_cfg_kv32_t) { keys->msm4_key, level == RTCM_MSM4 ? 1u : 0u };
    kvs[(*n)++] = (ubx_cfg_kv32_t) { keys->msm7_key, level == RTCM_MSM7 ? 1u : 0u };
}

esp_err_t gnss_ubx_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore u-blox come base RTK (Survey-In + RTCM3 su UART1)");

    app_settings_t s = settings_get();

    ubx_cfg_kv32_t kvs[32];
    size_t n = 0;
    kvs[n++] = (ubx_cfg_kv32_t) { 0x10740004, 1 }; // CFG-UART1OUTPROT-RTCM3X: abilita RTCM3 in uscita su UART1
    kvs[n++] = (ubx_cfg_kv32_t) { 0x10740002, 0 }; // CFG-UART1OUTPROT-NMEA: disabilita NMEA in uscita (solo RTCM3 su questa porta)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x209102be, s.rtcm_1005_enable ? 1u : 0u }; // CFG-MSGOUT-RTCM_3X_TYPE1005_UART1 (posizione base)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x20910304, s.rtcm_1230_enable ? 1u : 0u }; // CFG-MSGOUT-RTCM_3X_TYPE1230_UART1 (bias GLONASS)
    add_msm_level(kvs, &n, 32, &UBX_MSM_KEYS_GPS, s.rtcm_gps_msm);
    add_msm_level(kvs, &n, 32, &UBX_MSM_KEYS_GLONASS, s.rtcm_glonass_msm);
    add_msm_level(kvs, &n, 32, &UBX_MSM_KEYS_GALILEO, s.rtcm_galileo_msm);
    add_msm_level(kvs, &n, 32, &UBX_MSM_KEYS_BEIDOU, s.rtcm_beidou_msm);
    kvs[n++] = (ubx_cfg_kv32_t) { 0x20030001, 1 };     // CFG-TMODE-MODE = 1 (Survey-In)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x40030010, 60 };    // CFG-TMODE-SVIN-MIN-DUR: durata minima survey-in (s) - chiave corretta, era scambiata con quella sotto
    kvs[n++] = (ubx_cfg_kv32_t) { 0x40030011, 2500 };  // CFG-TMODE-SVIN-ACC-LIMIT: precisione richiesta, unita' 0.1mm (2500 = 250mm) - chiave corretta, era scambiata con quella sopra

    esp_err_t err = ubx_valset(uart_num, kvs, n);
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
