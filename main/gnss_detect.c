#include "gnss_detect.h"
#include "gnss_unicore.h"
#include "gnss_comnav.h"

#include <stdio.h>
#include <string.h>

#include "driver/uart.h"
#include "esp_log.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "gnss_detect";

#define NVS_NS        "gnssdet"
#define LISTEN_MS     800   // risposta attesa dopo le richieste, per velocita'
#define BUF_SIZE      1536

static gnss_chip_t s_detected = GNSS_CHIP_AUTO;
static char s_note[96];

// Statico: il task main ha gia' app_settings_t sullo stack.
static uint8_t s_buf[BUF_SIZE];

// UBX-MON-VER (classe 0x0A, id 0x04, nessun payload), checksum gia' calcolato.
static const uint8_t UBX_MON_VER_POLL[] = { 0xB5, 0x62, 0x0A, 0x04, 0x00, 0x00, 0x0E, 0x34 };

static const uint8_t *find(const uint8_t *buf, size_t len, const char *what)
{
    size_t n = strlen(what);
    for (size_t i = 0; i + n <= len; i++) {
        if (memcmp(buf + i, what, n) == 0) {
            return buf + i;
        }
    }
    return NULL;
}

// u-blox: risposta a MON-VER (con "MOD=..." tra le estensioni) o comunque una
// testata UBX con una classe nota (NAV, RXM, INF, ACK, CFG, MON, TIM, ESF).
static bool looks_ublox(const uint8_t *buf, size_t len, char *model, size_t model_size)
{
    const uint8_t *mod = find(buf, len, "MOD=");
    if (mod) {
        size_t k = 0;
        for (const uint8_t *p = mod + 4; p < buf + len && *p >= 0x20 && *p < 0x7F && k < model_size - 1; p++) {
            model[k++] = (char) *p;
        }
        model[k] = '\0';
        return true;
    }
    for (size_t i = 0; i + 3 < len; i++) {
        if (buf[i] == 0xB5 && buf[i + 1] == 0x62) {
            uint8_t cls = buf[i + 2];
            if (cls == 0x01 || cls == 0x02 || cls == 0x04 || cls == 0x05 || cls == 0x06 ||
                cls == 0x0A || cls == 0x0D || cls == 0x10) {
                return true;
            }
        }
    }
    return false;
}

// Unicore: riga "#VERSIONA,...;"UM982",...", passata a gnss_unicore.c.
static bool looks_unicore(const uint8_t *buf, size_t len)
{
    const uint8_t *p = find(buf, len, "#VERSIONA");
    if (!p) {
        return false;
    }
    static char line[256];
    size_t k = 0;
    for (; p < buf + len && *p != '\r' && *p != '\n' && k < sizeof(line) - 1; p++) {
        line[k++] = (char) *p;
    }
    line[k] = '\0';
    gnss_unicore_note_line(line);
    gnss_comnav_note_line(line);
    return strchr(line, ';') != NULL && gnss_unicore_model()[0];
}

// Una velocita': richieste u-blox e Unicore, poi ascolto. -1 = nessuno.
static int probe_baud(uart_port_t u, int baud, char *model, size_t model_size, bool *nmea_seen)
{
    uart_set_baudrate(u, (uint32_t) baud);
    vTaskDelay(pdMS_TO_TICKS(20));
    uart_flush_input(u);
    uart_write_bytes(u, (const char *) UBX_MON_VER_POLL, sizeof(UBX_MON_VER_POLL));
    uart_write_bytes(u, "\r\nVERSIONA\r\n", 12);
    uart_write_bytes(u, "LOG VERSIONA ONCE\r\n", 19); // ComNav (stile NovAtel)

    size_t len = 0;
    TickType_t end = xTaskGetTickCount() + pdMS_TO_TICKS(LISTEN_MS);
    while (len < sizeof(s_buf) && xTaskGetTickCount() < end) {
        int n = uart_read_bytes(u, s_buf + len, sizeof(s_buf) - len, pdMS_TO_TICKS(50));
        if (n > 0) {
            len += (size_t) n;
        }
    }
    if (looks_unicore(s_buf, len)) {
        strlcpy(model, gnss_unicore_model(), model_size);
        return GNSS_CHIP_UNICORE;
    }
    if (gnss_comnav_looks_comnav()) { // letto da looks_unicore() dalla stessa riga #VERSIONA
        strlcpy(model, gnss_comnav_model(), model_size);
        return GNSS_CHIP_COMNAV;
    }
    if (looks_ublox(s_buf, len, model, model_size)) {
        return GNSS_CHIP_UBLOX;
    }
    if (find(s_buf, len, "$G")) {
        *nmea_seen = true; // velocita' giusta, ma ricevitore non riconosciuto
    }
    return -1;
}

static void save_cache(gnss_chip_t chip, int baud)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "chip", (uint8_t) chip);
        nvs_set_i32(h, "baud", baud);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Seriale: velocita' delle impostazioni, poi quella dell'ultimo ricevitore
// trovato, poi le piu' comuni (UM98x di fabbrica 115200, u-blox 38400).
static int scan_uart(const app_settings_t *boot, int *found_baud, char *model, size_t model_size, bool *nmea_seen)
{
    int32_t cached = 0;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_i32(h, "baud", &cached);
        nvs_close(h);
    }
    const int common[] = { 115200, 38400, 230400, 460800, 921600, 9600 };
    int bauds[2 + sizeof(common) / sizeof(common[0])];
    size_t nb = 0;
    bauds[nb++] = boot->gnss_uart_baud > 0 ? boot->gnss_uart_baud : 115200;
    if (cached > 0 && cached != bauds[0]) {
        bauds[nb++] = (int) cached;
    }
    for (size_t i = 0; i < sizeof(common) / sizeof(common[0]); i++) {
        bool dup = false;
        for (size_t j = 0; j < nb; j++) dup |= bauds[j] == common[i];
        if (!dup) bauds[nb++] = common[i];
    }

    uart_port_t u = (uart_port_t) boot->gnss_uart_num;
    uart_config_t cfg = {
        .baud_rate = bauds[0],
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    if (uart_driver_install(u, 2048, 0, 0, NULL, 0) != ESP_OK) {
        ESP_LOGE(TAG, "UART%d non disponibile per il riconoscimento", (int) u);
        return -1;
    }
    uart_param_config(u, &cfg);
    uart_set_pin(u, boot->gnss_uart_tx_pin, boot->gnss_uart_rx_pin, UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE);

    int chip = -1;
    for (size_t i = 0; i < nb && chip < 0; i++) {
        chip = probe_baud(u, bauds[i], model, model_size, nmea_seen);
        if (chip >= 0) {
            *found_baud = bauds[i];
        }
    }
    uart_driver_delete(u); // gnss_io_init() la reinstalla con la velocita' trovata
    return chip;
}

void gnss_detect_run(app_settings_t *boot, bool i2c_found)
{
    char model[32] = "";
    if (boot->gnss_i2c && i2c_found) {
        // L'I2C 0x42 e' solo u-blox: il modello lo legge poi gnss_ubx.c.
        s_detected = GNSS_CHIP_UBLOX;
        snprintf(s_note, sizeof(s_note), "u-blox su I2C (0x42)");
    } else {
        int baud = 0;
        bool nmea_seen = false;
        int chip = scan_uart(boot, &baud, model, sizeof(model), &nmea_seen);
        if (chip >= 0) {
            s_detected = (gnss_chip_t) chip;
            boot->gnss_uart_baud = baud;
            boot->gnss_i2c = false;
            snprintf(s_note, sizeof(s_note), "%s%s%s su seriale a %d baud",
                     chip == GNSS_CHIP_UNICORE ? "Unicore" : (chip == GNSS_CHIP_COMNAV ? "ComNav" : "u-blox"),
                     model[0] ? " " : "", model, baud);
            save_cache(s_detected, baud);
        } else if (i2c_found) {
            s_detected = GNSS_CHIP_UBLOX;
            boot->gnss_i2c = true;
            snprintf(s_note, sizeof(s_note), "u-blox su I2C (0x42)");
        } else {
            // Come prima di questa funzione: si prova come u-blox.
            s_detected = GNSS_CHIP_UBLOX;
            snprintf(s_note, sizeof(s_note), nmea_seen
                     ? "ricevitore che manda NMEA ma non riconosciuto: sceglilo a mano"
                     : "nessun ricevitore ha risposto: controlla i collegamenti");
        }
    }
    boot->gnss_chip = s_detected;
    ESP_LOGI(TAG, "Riconoscimento del ricevitore: %s", s_note);
}

gnss_chip_t gnss_detect_effective(gnss_chip_t configured)
{
    if (configured != GNSS_CHIP_AUTO) {
        return configured;
    }
    return s_detected == GNSS_CHIP_AUTO ? GNSS_CHIP_UBLOX : s_detected;
}

const char *gnss_detect_note(void)
{
    return s_note;
}
