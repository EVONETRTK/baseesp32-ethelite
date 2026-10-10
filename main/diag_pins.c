#include "diag_pins.h"

#include <stdio.h>
#include <string.h>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_timer.h"
#include "soc/gpio_periph.h"
#include "soc/gpio_reg.h"
#include "soc/gpio_sig_map.h"
#include "soc/io_mux_reg.h"
#include "soc/soc.h"

#include "gnss_io.h"

#define SAMPLE_US 300000 // finestra di conteggio delle commutazioni
#define BURST_LEN 128    // raffica di prova sulla seriale (0x55: un fronte per bit)

static bool pin_valid(int pin)
{
    return pin >= 0 && pin < SOC_GPIO_PIN_COUNT;
}

static int pad_level(int pin)
{
    uint32_t v = pin < 32 ? REG_READ(GPIO_IN_REG) : REG_READ(GPIO_IN1_REG);
    return (int) ((v >> (pin & 31)) & 1);
}

static bool pad_out_enabled(int pin)
{
    uint32_t v = pin < 32 ? REG_READ(GPIO_ENABLE_REG) : REG_READ(GPIO_ENABLE1_REG);
    return (v >> (pin & 31)) & 1;
}

// Stato di un pin: funzione IO_MUX, ingresso abilitato, segnale d'uscita
// della matrice GPIO, uscita abilitata, livello attuale.
static cJSON *pin_info(int pin)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "gpio", pin);
    if (!pin_valid(pin)) {
        return o;
    }
    uint32_t mux = REG_READ(GPIO_PIN_MUX_REG[pin]);
    uint32_t out = REG_READ(GPIO_FUNC0_OUT_SEL_CFG_REG + 4 * pin);
    cJSON_AddNumberToObject(o, "iomux_func", (mux >> MCU_SEL_S) & MCU_SEL_V);
    cJSON_AddBoolToObject(o, "input_enabled", (mux & FUN_IE) != 0);
    cJSON_AddNumberToObject(o, "matrix_out_signal", out & 0x1FF); // 256 = uscita GPIO semplice
    cJSON_AddNumberToObject(o, "matrix_out_raw", out);
    cJSON_AddBoolToObject(o, "output_enabled", pad_out_enabled(pin));
    cJSON_AddNumberToObject(o, "level", pad_level(pin));
    return o;
}

// Quale pin la matrice GPIO collega all'ingresso RX di una UART.
static cJSON *in_sel(int signal)
{
    uint32_t v = REG_READ(GPIO_FUNC0_IN_SEL_CFG_REG + 4 * signal);
    cJSON *o = cJSON_CreateObject();
    cJSON_AddNumberToObject(o, "gpio", v & 0x3F);
    cJSON_AddBoolToObject(o, "inverted", (v >> 6) & 1);
    cJSON_AddBoolToObject(o, "via_matrix", (v >> 7) & 1);
    return o;
}

void diag_pins_gnss(cJSON *root, int tx_pin, int rx_pin)
{
    cJSON_AddItemToObject(root, "tx", pin_info(tx_pin));
    cJSON_AddItemToObject(root, "rx", pin_info(rx_pin));

    cJSON *sig = cJSON_AddObjectToObject(root, "uart_signals");
    cJSON_AddNumberToObject(sig, "u0txd", U0TXD_OUT_IDX);
    cJSON_AddNumberToObject(sig, "u1txd", U1TXD_OUT_IDX);
    cJSON_AddNumberToObject(sig, "u2txd", U2TXD_OUT_IDX);
    cJSON_AddItemToObject(sig, "u0rxd_from", in_sel(U0RXD_IN_IDX));
    cJSON_AddItemToObject(sig, "u1rxd_from", in_sel(U1RXD_IN_IDX));
    cJSON_AddItemToObject(sig, "u2rxd_from", in_sel(U2RXD_IN_IDX));

    if (!pin_valid(tx_pin) || !pin_valid(rx_pin)) {
        cJSON_AddStringToObject(root, "note", "pin non validi");
        return;
    }

    // Per leggere il livello del pin TX serve l'ingresso del pad abilitato:
    // non tocca l'uscita (solo lettura).
    PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[tx_pin]);

    bool uart = !gnss_io_is_i2c();
    int burst = -1;
    if (uart) {
        uint8_t b[BURST_LEN];
        memset(b, 0x55, sizeof(b));
        burst = gnss_io_write(b, sizeof(b));
    }

    int last_tx = pad_level(tx_pin), last_rx = pad_level(rx_pin);
    int edges_tx = 0, edges_rx = 0, low_tx = 0, low_rx = 0, samples = 0;
    int64_t end = esp_timer_get_time() + SAMPLE_US;
    while (esp_timer_get_time() < end) {
        int t = pad_level(tx_pin), r = pad_level(rx_pin);
        edges_tx += t != last_tx;
        edges_rx += r != last_rx;
        low_tx += t == 0;
        low_rx += r == 0;
        last_tx = t;
        last_rx = r;
        samples++;
    }

    cJSON *w = cJSON_AddObjectToObject(root, "window");
    cJSON_AddNumberToObject(w, "ms", SAMPLE_US / 1000);
    cJSON_AddNumberToObject(w, "samples", samples);
    cJSON_AddBoolToObject(w, "uart_link", uart);
    cJSON_AddNumberToObject(w, "burst_written", burst);
    cJSON_AddNumberToObject(w, "tx_edges", edges_tx);
    cJSON_AddNumberToObject(w, "rx_edges", edges_rx);
    cJSON_AddNumberToObject(w, "tx_low_samples", low_tx);
    cJSON_AddNumberToObject(w, "rx_low_samples", low_rx);

    // Tutti i pin leggibili (esclusi USB 19/20 e flash/PSRAM 26-37): su
    // quale arrivano commutazioni. Un ricevitore che trasmette di continuo
    // si riconosce da migliaia di fronti. Abilitare l'ingresso non cambia
    // l'uscita dei pin gia' in uso.
    static uint16_t edges[SOC_GPIO_PIN_COUNT];
    memset(edges, 0, sizeof(edges));
    for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
        // 22-25 non esistono sull'S3 (registro IO_MUX nullo: scriverci blocca il chip).
        if (!GPIO_IS_VALID_GPIO(p) || GPIO_PIN_MUX_REG[p] == 0 || p == 19 || p == 20 || (p >= 26 && p <= 37)) {
            continue;
        }
        PIN_INPUT_ENABLE(GPIO_PIN_MUX_REG[p]);
    }
    uint32_t prev0 = REG_READ(GPIO_IN_REG), prev1 = REG_READ(GPIO_IN1_REG);
    end = esp_timer_get_time() + SAMPLE_US;
    while (esp_timer_get_time() < end) {
        uint32_t c0 = REG_READ(GPIO_IN_REG), c1 = REG_READ(GPIO_IN1_REG);
        uint32_t d0 = c0 ^ prev0, d1 = c1 ^ prev1;
        while (d0) {
            int b = __builtin_ctz(d0);
            if (edges[b] < UINT16_MAX) edges[b]++;
            d0 &= d0 - 1;
        }
        while (d1) {
            int b = __builtin_ctz(d1);
            if (32 + b < SOC_GPIO_PIN_COUNT && edges[32 + b] < UINT16_MAX) edges[32 + b]++;
            d1 &= d1 - 1;
        }
        prev0 = c0;
        prev1 = c1;
    }
    cJSON *scan = cJSON_AddObjectToObject(root, "edges_all_pins");
    for (int p = 0; p < SOC_GPIO_PIN_COUNT; p++) {
        if (edges[p]) {
            char k[8];
            snprintf(k, sizeof(k), "%d", p);
            cJSON_AddNumberToObject(scan, k, edges[p]);
        }
    }
}
