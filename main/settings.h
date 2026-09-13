#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    GNSS_CHIP_UBLOX = 0,
    GNSS_CHIP_UNICORE = 1,
} gnss_chip_t;

typedef enum {
    DEVICE_MODE_BASE = 0,
    DEVICE_MODE_ROVER = 1,
} device_mode_t;

typedef enum {
    NETWORK_MODE_WIFI_ONLY = 0,     // mai il modem cellulare, anche se collegato
    NETWORK_MODE_CELLULAR_ONLY = 1, // mai il WiFi station (l'AP di setup resta comunque attivo)
    NETWORK_MODE_BOTH = 2,          // WiFi preferito, fallback automatico su GPRS
} network_mode_t;

typedef enum {
    RGB_LED_NONE = 0,   // nessun LED RGB collegato
    RGB_LED_WS2812 = 1, // indirizzabile, un solo pin dati (driver RMT)
    RGB_LED_PWM3 = 2,   // 3 pin separati R/G/B, intensita' via PWM (LEDC)
} rgb_led_mode_t;

typedef struct {
    char wifi_ssid[33];
    char wifi_password[65];
    char cellular_apn[64];
    network_mode_t network_mode;
    // Lo slot LTE della T-ETH-Elite e' elettricamente lo stesso (TX/RX/
    // PWRKEY/DTR fissi) per qualunque shield compatibile ci si innesti -
    // cambia pero' il profilo AT/sequenza di accensione da usare secondo
    // il modulo fisico montato. SIM7600X e' l'hardware target scelto per
    // questo progetto; SIM868 e' qui solo per riutilizzare/testare il
    // modulo del progetto gemello baseesp32/T-Internet-COM (li' risultato
    // difettoso hardware - utile verificare se il difetto era del modulo
    // o dello slot di quella scheda).
    bool cellular_is_sim868;
    char ntrip_host[64];
    uint16_t ntrip_port;
    char ntrip_mountpoint[33];
    char ntrip_username[33];  // usato solo in modalita' rover (NTRIP GET, Basic Auth)
    char ntrip_password[64];
    gnss_chip_t gnss_chip;
    device_mode_t device_mode;
    char ap_ssid[33];
    char ap_password[65];
    char admin_code[33];      // protegge la UI web (HTTP Basic Auth, utente fisso "admin")
    // Matricola assegnata all'unita' fisica (tracciamento/assistenza) - per
    // default il MAC address di fabbrica del chip (identificativo unico
    // reale, diverso dal solo suffisso usato per l'SSID), sovrascrivibile
    // dalla UI web con un proprio codice se lo si preferisce.
    char device_serial[33];
    uint16_t nmea_udp_port;   // porta broadcast UDP per NMEA (rover), es. per AgOpenGPS/AgIO

    // Pin verso il modulo GNSS esterno (u-blox/Unicore): configurabili a
    // runtime, a differenza dei pin del modem/Ethernet che sono fissati
    // dalla scheda/shield e restano solo in Kconfig. Qui variano davvero
    // installazione per installazione a seconda di come si cablano.
    int gnss_uart_num;
    int gnss_uart_tx_pin;
    int gnss_uart_rx_pin;
    int gnss_uart_baud;

    // LED RGB opzionale (in aggiunta ai LED semplici rete/dati, gia'
    // solo-Kconfig). -1 su un pin = non usato/non applicabile alla
    // modalita' scelta.
    rgb_led_mode_t rgb_led_mode;
    int rgb_led_ws2812_pin;
    int rgb_led_pwm_r_pin;
    int rgb_led_pwm_g_pin;
    int rgb_led_pwm_b_pin;
    bool rgb_led_pwm_active_low;

    // Display OLED opzionale (128x64 monocromo, via I2C). -1 su sda/scl =
    // non presente. Alterna a rotazione stato/satelliti per
    // costellazione/segnale cellulare e WiFi.
    int oled_sda_pin;
    int oled_scl_pin;
    uint8_t oled_i2c_addr; // tipicamente 0x3C, alcuni cloni 0x3D
    // I moduli da 0,96" montano quasi sempre SSD1306; quelli da 1,3" quasi
    // sempre SH1106 (RAM interna 132x64, comandi di indirizzamento diversi
    // - serve un driver leggermente diverso, non e' solo una questione di
    // dimensione fisica).
    bool oled_is_sh1106;
    // Orientamento: dipende da come il singolo modulo ha cablato
    // SEG/COM al vetro, varia da produttore a produttore - non c'e' un
    // valore giusto universale, va provato. flip_h risolve testo/immagine
    // "a specchio" (orizzontale), flip_v un display sottosopra (verticale).
    bool oled_flip_h;
    bool oled_flip_v;

    // URL di un piccolo manifest JSON ({"version":"x.y.z","url":"..."})
    // per l'aggiornamento "online" - tipicamente un asset di una release
    // GitHub. Vuoto = funzione non configurata/disattivata.
    char ota_update_url[128];
} app_settings_t;

// Carica la configurazione da NVS; se assente o non valida usa i default
// da Kconfig (SSID AP generato dal MAC del dispositivo). Va chiamata una
// sola volta all'avvio, dopo nvs_flash_init().
void settings_init(void);

// Ritorna una copia della configurazione corrente (struct piccola, copia
// per evitare di dover gestire un lock condiviso tra i vari task).
app_settings_t settings_get(void);

// Sovrascrive e persiste la configurazione su NVS. I campi stringa vuoti
// passati dal chiamante vanno gestiti a monte (es. web_ui.c non sovrascrive
// una password esistente con una stringa vuota).
esp_err_t settings_save(const app_settings_t *s);
