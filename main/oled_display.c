#include "oled_display.h"
#include "settings.h"
#include "status.h"
#include "gnss_signal.h"
#include "wifi_link.h"
#include "cellular_link.h"
#include "font8x8_basic.h"

#include <string.h>
#include <stdio.h>
#include <stdbool.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "driver/i2c_master.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "oled_display";

#define OLED_W 128
#define OLED_H 64
#define OLED_PAGES (OLED_H / 8)

// Offset di colonna per SH1106: RAM interna 132x64, ma solo 128 colonne
// sono cablate al vetro visibile - valore usato dalla stragrande
// maggioranza dei driver SH1106 in circolazione (es. u8g2). Puramente
// estetico se leggermente sbagliato per un modulo particolare (immagine
// spostata di qualche pixel, non corrotta).
#define SH1106_COL_OFFSET 2

static i2c_master_dev_handle_t s_dev = NULL;
static uint8_t s_fb[OLED_W * OLED_PAGES];
static bool s_is_sh1106 = false;

static esp_err_t oled_cmd(uint8_t cmd)
{
    uint8_t buf[2] = { 0x00, cmd };
    return i2c_master_transmit(s_dev, buf, sizeof(buf), 100);
}

static esp_err_t oled_cmds(const uint8_t *cmds, size_t n)
{
    for (size_t i = 0; i < n; i++) {
        esp_err_t err = oled_cmd(cmds[i]);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static void fb_clear(void)
{
    memset(s_fb, 0, sizeof(s_fb));
}

static void fb_set_pixel(int x, int y, bool on)
{
    if (x < 0 || x >= OLED_W || y < 0 || y >= OLED_H) {
        return;
    }
    uint8_t *byte = &s_fb[(y / 8) * OLED_W + x];
    uint8_t mask = 1 << (y % 8);
    if (on) {
        *byte |= mask;
    } else {
        *byte &= (uint8_t) ~mask;
    }
}

static void fb_fill_rect(int x, int y, int w, int h)
{
    for (int dy = 0; dy < h; dy++) {
        for (int dx = 0; dx < w; dx++) {
            fb_set_pixel(x + dx, y + dy, true);
        }
    }
}

static void fb_draw_rect(int x, int y, int w, int h)
{
    for (int dx = 0; dx < w; dx++) {
        fb_set_pixel(x + dx, y, true);
        fb_set_pixel(x + dx, y + h - 1, true);
    }
    for (int dy = 0; dy < h; dy++) {
        fb_set_pixel(x, y + dy, true);
        fb_set_pixel(x + w - 1, y + dy, true);
    }
}

// Disegna un carattere 8x8 (font8x8_basic.h, dominio pubblico - vedi
// commento in quel file): ogni byte del glifo e' una colonna, bit 0 =
// riga superiore. Angolo in alto a sinistra del carattere in (x,y).
static void fb_draw_char(int x, int y, char c)
{
    if ((unsigned char) c > 0x7F) {
        return;
    }
    const uint8_t *glyph = font8x8_basic[(unsigned char) c];
    for (int col = 0; col < 8; col++) {
        uint8_t bits = glyph[col];
        for (int row = 0; row < 8; row++) {
            if (bits & (1 << row)) {
                fb_set_pixel(x + col, y + row, true);
            }
        }
    }
}

static void fb_draw_text(int x, int y, const char *text)
{
    int cx = x;
    for (const char *p = text; *p; p++) {
        fb_draw_char(cx, y, *p);
        cx += 8;
    }
}

// "Tacche di segnale" in stile telefono: num_bars colonne di altezza
// crescente allineate sulla base (baseline_y), le prime filled_bars piene,
// le restanti solo contorno.
static void fb_draw_ladder(int x, int baseline_y, int num_bars, int bar_w, int gap,
                            int max_h, int filled_bars)
{
    for (int i = 0; i < num_bars; i++) {
        int h = max_h * (i + 1) / num_bars;
        int bx = x + i * (bar_w + gap);
        int by = baseline_y - h;
        if (i < filled_bars) {
            fb_fill_rect(bx, by, bar_w, h);
        } else {
            fb_draw_rect(bx, by, bar_w, h);
        }
    }
}

// SSD1306 supporta l'indirizzamento orizzontale automatico (0x21/0x22 +
// scrittura contigua di tutto il framebuffer). SH1106 non ha questo
// comando: RAM interna 132x64 indirizzata una pagina (8 righe) alla
// volta, via 0xB0+pagina e nibble basso/alto della colonna di partenza.
static esp_err_t oled_flush_ssd1306(void)
{
    static const uint8_t addr_cmds[] = {
        0x21, 0x00, OLED_W - 1,     // column address range
        0x22, 0x00, OLED_PAGES - 1, // page address range
    };
    esp_err_t err = oled_cmds(addr_cmds, sizeof(addr_cmds));
    if (err != ESP_OK) {
        return err;
    }

    uint8_t chunk[65];
    chunk[0] = 0x40; // control byte: dati (non comando)
    for (size_t off = 0; off < sizeof(s_fb); off += 64) {
        size_t n = sizeof(s_fb) - off < 64 ? sizeof(s_fb) - off : 64;
        memcpy(chunk + 1, s_fb + off, n);
        err = i2c_master_transmit(s_dev, chunk, n + 1, 100);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t oled_flush_sh1106(void)
{
    uint8_t chunk[OLED_W + 1];
    chunk[0] = 0x40; // control byte: dati (non comando)

    for (int page = 0; page < OLED_PAGES; page++) {
        uint8_t page_cmds[3] = {
            (uint8_t) (0xB0 + page),
            (uint8_t) (0x00 | (SH1106_COL_OFFSET & 0x0F)),
            (uint8_t) (0x10 | (SH1106_COL_OFFSET >> 4)),
        };
        esp_err_t err = oled_cmds(page_cmds, sizeof(page_cmds));
        if (err != ESP_OK) {
            return err;
        }
        memcpy(chunk + 1, s_fb + page * OLED_W, OLED_W);
        err = i2c_master_transmit(s_dev, chunk, sizeof(chunk), 100);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

static esp_err_t oled_flush(void)
{
    return s_is_sh1106 ? oled_flush_sh1106() : oled_flush_ssd1306();
}

static bool oled_hw_init(int sda_pin, int scl_pin, uint8_t addr, bool is_sh1106,
                          bool flip_h, bool flip_v)
{
    s_is_sh1106 = is_sh1106;

    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    i2c_master_bus_handle_t bus;
    if (i2c_new_master_bus(&bus_cfg, &bus) != ESP_OK) {
        ESP_LOGE(TAG, "Init bus I2C fallita (SDA=%d SCL=%d)", sda_pin, scl_pin);
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = 400000,
    };
    if (i2c_master_bus_add_device(bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "Aggiunta device I2C OLED (0x%02X) fallita", addr);
        return false;
    }

    // Sequenza di init standard, la stessa (a meno di dettagli minori)
    // usata in praticamente ogni driver esistente per questi controller -
    // a differenza del font, qui un eventuale errore di battitura si
    // manifesta come "display non si accende affatto", facile da
    // diagnosticare, non come testo/grafica corrotti silenziosamente.
    // SSD1306 e SH1106 differiscono sul comando del charge pump (0x8D/0x14
    // contro 0xAD/0x8B) e SH1106 non ha il comando "memory addressing
    // mode" (0x20) - usa solo indirizzamento a pagine, vedi oled_flush().
    // L'orientamento (segment remap 0xA0/0xA1, direzione scansione COM
    // 0xC0/0xC8) dipende da come il singolo modulo ha cablato SEG/COM al
    // vetro - non c'e' un valore giusto universale, va provato (flip_h/
    // flip_v, configurabili dalla UI web senza ricompilare).
    uint8_t seq[28]; // max effettivo 25 byte (ramo SSD1306), margine incluso
    size_t n = 0;
    seq[n++] = 0xAE;                         // display off
    seq[n++] = 0xD5; seq[n++] = 0x80;        // clock divider / frequenza oscillatore
    seq[n++] = 0xA8; seq[n++] = 0x3F;        // multiplex ratio = 64
    seq[n++] = 0xD3; seq[n++] = 0x00;        // display offset = 0
    seq[n++] = 0x40;                         // start line = 0
    if (is_sh1106) {
        seq[n++] = 0xAD; seq[n++] = 0x8B;    // charge pump DC-DC abilitato (SH1106)
    } else {
        seq[n++] = 0x8D; seq[n++] = 0x14;    // charge pump abilitato (SSD1306)
        seq[n++] = 0x20; seq[n++] = 0x00;    // memory addressing mode = orizzontale
    }
    seq[n++] = flip_h ? 0xA1 : 0xA0;         // segment remap
    seq[n++] = flip_v ? 0xC8 : 0xC0;         // direzione scansione COM
    seq[n++] = 0xDA; seq[n++] = 0x12;        // configurazione hardware pin COM
    seq[n++] = 0x81; seq[n++] = 0xCF;        // contrasto
    seq[n++] = 0xD9; seq[n++] = 0xF1;        // periodo di pre-carica
    seq[n++] = 0xDB; seq[n++] = 0x40;        // livello di deselezione VCOMH
    seq[n++] = 0xA4;                         // riprendi contenuto RAM
    seq[n++] = 0xA6;                         // display normale (non invertito)
    seq[n++] = 0xAF;                         // display on

    if (oled_cmds(seq, n) != ESP_OK) {
        ESP_LOGE(TAG, "Sequenza di init OLED fallita: display assente o non risponde su 0x%02X", addr);
        return false;
    }

    fb_clear();
    oled_flush();
    return true;
}

static const char *net_label(net_status_t s)
{
    switch (s) {
    case NET_STATUS_WIFI:     return "NET: WIFI";
    case NET_STATUS_CELLULAR: return "NET: CELL";
    default:                  return "NET: ---";
    }
}

static void format_uptime(char *out, size_t out_size)
{
    int64_t s = esp_timer_get_time() / 1000000;
    int h = (int) (s / 3600);
    int m = (int) ((s % 3600) / 60);
    int sec = (int) (s % 60);
    snprintf(out, out_size, "UP %02d:%02d:%02d", h, m, sec);
}

static void screen_status(void)
{
    app_settings_t s = settings_get();
    fb_draw_text(0, 0, s.device_mode == DEVICE_MODE_ROVER ? "ROVER" : "BASE");
    fb_draw_text(0, 16, net_label(status_get_net()));

    char line[24];
    snprintf(line, sizeof(line), "RTCM %lu B", (unsigned long) status_get_rtcm_total_bytes());
    fb_draw_text(0, 32, line);

    format_uptime(line, sizeof(line));
    fb_draw_text(0, 48, line);
}

// Conteggio satelliti in vista per costellazione (GP/GL/GA/GB + altro) -
// stesso significato gia' usato dal grafico della UI web (gnss_signal.c
// tiene traccia dei satelliti riportati dalle sentenze GSV, non solo
// quelli usati nel fix).
static void screen_gnss(void)
{
    fb_draw_text(0, 0, "SATELLITI");

    gnss_sat_signal_t sats[GNSS_SIGNAL_MAX_SATS];
    size_t n = gnss_signal_get_satellites(sats, GNSS_SIGNAL_MAX_SATS);

    int gp = 0, gl = 0, ga = 0, gb = 0, other = 0;
    for (size_t i = 0; i < n; i++) {
        if (memcmp(sats[i].constellation, "GP", 2) == 0) gp++;
        else if (memcmp(sats[i].constellation, "GL", 2) == 0) gl++;
        else if (memcmp(sats[i].constellation, "GA", 2) == 0) ga++;
        else if (memcmp(sats[i].constellation, "GB", 2) == 0) gb++;
        else other++;
    }

    const char *labels[5] = { "GP", "GL", "GA", "GB", "OT" };
    int counts[5] = { gp, gl, ga, gb, other };
    const int max_count = 16; // scala fissa: barra piena = 16 satelliti in vista

    for (int i = 0; i < 5; i++) {
        int col_x = i * 25;
        int h = counts[i] > max_count ? 40 : (40 * counts[i] / max_count);
        if (h < 1 && counts[i] > 0) {
            h = 1;
        }
        if (h > 0) {
            fb_fill_rect(col_x + 4, 62 - h, 12, h);
        }
        fb_draw_text(col_x, 18, labels[i]);

        char num[4];
        snprintf(num, sizeof(num), "%d", counts[i]);
        fb_draw_text(col_x + (counts[i] >= 10 ? 0 : 4), 10, num);
    }
}

static void screen_signals(void)
{
    fb_draw_text(0, 0, "SEGNALE");

    int8_t wifi_rssi = 0;
    bool wifi_ok = wifi_link_get_rssi(&wifi_rssi);
    int wifi_filled = 0;
    if (wifi_ok) {
        int pct = wifi_rssi <= -90 ? 0 : (wifi_rssi >= -30 ? 100 : (wifi_rssi + 90) * 100 / 60);
        wifi_filled = (pct * 5 + 50) / 100;
    }
    fb_draw_text(0, 14, "WiFi");
    fb_draw_ladder(40, 30, 5, 5, 2, 16, wifi_filled);
    if (wifi_ok) {
        char wtxt[12];
        snprintf(wtxt, sizeof(wtxt), "%ddBm", wifi_rssi);
        fb_draw_text(75, 14, wtxt);
    } else {
        fb_draw_text(75, 14, "n/d");
    }

    int cell_rssi = 0;
    bool cell_ok = cellular_link_get_signal(&cell_rssi);
    int cell_filled = 0;
    if (cell_ok) {
        int pct = cell_rssi <= -113 ? 0 : (cell_rssi >= -51 ? 100 : (cell_rssi + 113) * 100 / 62);
        cell_filled = (pct * 5 + 50) / 100;
    }
    fb_draw_text(0, 40, "CELL");
    fb_draw_ladder(40, 56, 5, 5, 2, 16, cell_filled);

    char op[16] = { 0 };
    char tech[8] = { 0 };
    if (cellular_link_get_operator_info(op, sizeof(op), tech, sizeof(tech))) {
        char ctxt[24];
        snprintf(ctxt, sizeof(ctxt), "%.8s %s", op, tech);
        fb_draw_text(75, 40, ctxt);
    } else {
        fb_draw_text(75, 40, "n/d");
    }
}

// Schermata iniziale, mostrata una sola volta all'avvio del task (non
// blocca il resto del boot: il ritardo qui sotto e' solo nel task OLED).
static void screen_splash(void)
{
    fb_draw_text(40, 20, "EVONET");
    fb_draw_text(52, 36, "RTK");
}

static void oled_task(void *arg)
{
    fb_clear();
    screen_splash();
    oled_flush();
    vTaskDelay(pdMS_TO_TICKS(2500));

    int screen = 0;
    while (1) {
        fb_clear();
        switch (screen) {
        case 0: screen_status(); break;
        case 1: screen_gnss(); break;
        default: screen_signals(); break;
        }
        if (oled_flush() != ESP_OK) {
            ESP_LOGW(TAG, "Scrittura I2C verso OLED fallita (display scollegato?)");
        }
        screen = (screen + 1) % 3;
        vTaskDelay(pdMS_TO_TICKS(2500));
    }
}

void oled_display_start(void)
{
    app_settings_t s = settings_get();
    if (s.oled_sda_pin < 0 || s.oled_scl_pin < 0) {
        return;
    }
    // SSD1309 usa lo stesso percorso di SSD1306 nel driver hardware (vedi
    // commento su oled_controller_t in settings.h) - solo SH1106 richiede
    // la sequenza di comandi/indirizzamento diversa gestita da oled_hw_init().
    bool is_sh1106 = (s.oled_controller == OLED_CTRL_SH1106);
    if (!oled_hw_init(s.oled_sda_pin, s.oled_scl_pin, s.oled_i2c_addr, is_sh1106,
                       s.oled_flip_h, s.oled_flip_v)) {
        return;
    }
    const char *controller_str = (s.oled_controller == OLED_CTRL_SH1106) ? "SH1106"
                                : (s.oled_controller == OLED_CTRL_SSD1309) ? "SSD1309" : "SSD1306";
    xTaskCreate(oled_task, "oled_display", 4096, NULL, 2, NULL);
    ESP_LOGI(TAG, "Display OLED attivo (SDA=%d SCL=%d addr=0x%02X controller=%s flip_h=%d flip_v=%d)",
             s.oled_sda_pin, s.oled_scl_pin, s.oled_i2c_addr, controller_str,
             s.oled_flip_h, s.oled_flip_v);
}
