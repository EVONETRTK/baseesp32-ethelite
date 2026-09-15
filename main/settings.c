#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <stdlib.h>

#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"

#include "sdkconfig.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "baseesp32"
#define NVS_KEY_CFG   "cfg"
// "bs02", non "bs01": il magic e' stato cambiato apposta insieme
// all'introduzione del merge tollerante alle differenze di dimensione in
// settings_init() sotto. I blob salvati con "bs01" (qualunque versione di
// questo firmware precedente a quella che ha introdotto "bs02") possono
// avere campi non solo aggiunti in fondo ma anche cambiati di tipo/
// dimensione a parita' di posizione (es. un campo bool diventato un enum) -
// non e' sicuro fare merge parziale su quei byte, causerebbe valori
// corrotti nei campi successivi (visto in pratica: un uart_num letto come
// spazzatura ha mandato in crash il boot). Da "bs02" in poi la regola
// "solo aggiunte in fondo" (vedi commento su app_settings_t in settings.h)
// e' garantita, quindi il merge parziale e' sicuro.
#define CFG_MAGIC     0x62733032u // "bs02"

typedef struct {
    uint32_t magic;
    app_settings_t s;
} stored_cfg_t;

static app_settings_t s_settings;
// Praticamente ogni modulo di questo firmware chiama settings_get() (con
// che frequenza varia da task a task, alcuni periodicamente) mentre
// settings_get()/settings_save() copiano l'intera struct (>1.7KB, ~450
// word) SENZA alcuna protezione - su un chip dual-core con FreeRTOS
// preemptive, una copia cosi' grossa non e' atomica: un settings_save()
// da un task puo' essere interrotto a meta' da un altro task che nello
// stesso istante chiama settings_get(), risultando in una lettura "a
// pezzi" (alcuni campi vecchi, altri nuovi) - o viceversa. Ricostruito a
// posteriori come causa piu' probabile di una serie di corruzioni
// apparentemente casuali di singoli campi vista in questa sessione
// (valori spazzatura diversi ogni volta in campi diversi: gnss_uart_num,
// ota_update_url...) che i fix precedenti (rete di sicurezza sui singoli
// campi) attenuavano senza risolvere alla radice. Il mutex qui sotto
// rende atomica ogni lettura/scrittura dell'intera struct condivisa.
static SemaphoreHandle_t s_settings_mutex;

// Stessi 3 byte finali del MAC gia' usati per il suffisso dell'SSID
// dell'AP di setup (es. "EVONETRTK-893428" -> "893428") - stesso numero
// mostrato in due punti diversi della UI invece di due identificativi
// diversi da mettere in relazione a mano.
static void format_mac_serial(char *out, size_t out_size)
{
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(out, out_size, "%02X%02X%02X", mac[3], mac[4], mac[5]);
}

static void apply_defaults(void)
{
    memset(&s_settings, 0, sizeof(s_settings));

    strncpy(s_settings.wifi_ssid, CONFIG_BASEESP32_WIFI_SSID, sizeof(s_settings.wifi_ssid) - 1);
    strncpy(s_settings.wifi_password, CONFIG_BASEESP32_WIFI_PASSWORD, sizeof(s_settings.wifi_password) - 1);
#if CONFIG_BASEESP32_CELLULAR_ENABLE
    strncpy(s_settings.cellular_apn, CONFIG_BASEESP32_CELLULAR_APN, sizeof(s_settings.cellular_apn) - 1);
#endif
#ifdef CONFIG_BASEESP32_CELLULAR_MODEM_IS_SIM868
    s_settings.cellular_is_sim868 = true;
#else
    s_settings.cellular_is_sim868 = false;
#endif
    strncpy(s_settings.ntrip_host, CONFIG_BASEESP32_NTRIP_HOST, sizeof(s_settings.ntrip_host) - 1);
    s_settings.ntrip_port = CONFIG_BASEESP32_NTRIP_PORT;
    strncpy(s_settings.ntrip_mountpoint, CONFIG_BASEESP32_NTRIP_MOUNTPOINT, sizeof(s_settings.ntrip_mountpoint) - 1);
    strncpy(s_settings.ota_update_url, CONFIG_BASEESP32_OTA_UPDATE_URL, sizeof(s_settings.ota_update_url) - 1);
    s_settings.alert_enable = false;
    s_settings.alert_threshold_min = 15;
    s_settings.alert_smtp_port = 465;
    s_settings.base_drift_alert_enable = false;
    s_settings.base_drift_threshold_m = 5.0f;
    s_settings.ntrip_caster_server_enable = false;
    s_settings.ntrip_caster_server_port = 2101;
    strncpy(s_settings.ntrip_caster_server_mountpoint, "BASE01", sizeof(s_settings.ntrip_caster_server_mountpoint) - 1);
    s_settings.base_position_mode = BASE_POSITION_AUTO;
    s_settings.auto_update_check_enable = false;
    s_settings.auto_update_check_interval_h = 24;
    s_settings.gnss_chip = GNSS_CHIP_UBLOX;
    s_settings.device_mode = DEVICE_MODE_BASE;
    s_settings.network_mode = NETWORK_MODE_BOTH;
    s_settings.nmea_udp_port = 5005;

    s_settings.gnss_uart_num = CONFIG_BASEESP32_GNSS_UART_NUM;
    s_settings.gnss_uart_tx_pin = CONFIG_BASEESP32_GNSS_UART_TX_PIN;
    s_settings.gnss_uart_rx_pin = CONFIG_BASEESP32_GNSS_UART_RX_PIN;
    s_settings.gnss_uart_baud = CONFIG_BASEESP32_GNSS_UART_BAUD;

    s_settings.rgb_led_mode = RGB_LED_NONE;
    s_settings.rgb_led_ws2812_pin = CONFIG_BASEESP32_RGB_WS2812_PIN;
    s_settings.rgb_led_pwm_r_pin = CONFIG_BASEESP32_RGB_PWM_R_PIN;
    s_settings.rgb_led_pwm_g_pin = CONFIG_BASEESP32_RGB_PWM_G_PIN;
    s_settings.rgb_led_pwm_b_pin = CONFIG_BASEESP32_RGB_PWM_B_PIN;
    s_settings.rgb_led_pwm_active_low = false;

    s_settings.oled_sda_pin = CONFIG_BASEESP32_OLED_SDA_PIN;
    s_settings.oled_scl_pin = CONFIG_BASEESP32_OLED_SCL_PIN;
    s_settings.oled_i2c_addr = CONFIG_BASEESP32_OLED_I2C_ADDR;
#ifdef CONFIG_BASEESP32_OLED_CONTROLLER_SH1106
    s_settings.oled_controller = OLED_CTRL_SH1106;
#else
    s_settings.oled_controller = OLED_CTRL_SSD1306;
#endif
#ifdef CONFIG_BASEESP32_OLED_FLIP_H
    s_settings.oled_flip_h = true;
#else
    s_settings.oled_flip_h = false;
#endif
#ifdef CONFIG_BASEESP32_OLED_FLIP_V
    s_settings.oled_flip_v = true;
#else
    s_settings.oled_flip_v = false;
#endif

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_settings.ap_ssid, sizeof(s_settings.ap_ssid), "EVONETRTK-%02X%02X%02X", mac[3], mac[4], mac[5]);
    // Matricola di default = stesso suffisso usato sopra per l'SSID
    // dell'AP, per non avere due identificativi diversi da mettere in
    // relazione a mano - resta comunque sovrascrivibile dalla UI web.
    // Vedi anche settings_init() sotto per i dispositivi gia' provvisti di
    // una configurazione salvata precedente a questo campo.
    format_mac_serial(s_settings.device_serial, sizeof(s_settings.device_serial));
    strncpy(s_settings.ap_password, "baseesp32setup", sizeof(s_settings.ap_password) - 1);
    strncpy(s_settings.admin_code, "1234", sizeof(s_settings.admin_code) - 1);
}

void settings_init(void)
{
    // Creato qui (chiamata singola, sincrona, prima che qualunque altro
    // task sia avviato - vedi main.c) invece che pigramente dentro
    // settings_get()/settings_save(): evita la finestra in cui due task
    // potrebbero vedere entrambi il mutex non ancora creato e crearne due
    // copie diverse.
    s_settings_mutex = xSemaphoreCreateMutex();

    ESP_LOGI(TAG, "sizeof(app_settings_t) di questo firmware = %u byte", (unsigned) sizeof(app_settings_t));

    apply_defaults();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Nessuna configurazione salvata in NVS, uso i default di Kconfig");
        return;
    }

    // Lunghezza REALMENTE salvata, che puo' essere minore di sizeof(stored_cfg_t)
    // se il blob risale a una versione precedente del firmware con meno
    // campi in app_settings_t (ogni volta che si aggiunge un campo, come
    // successo piu' volte in questo progetto, la vecchia dimensione non
    // combacia piu' con quella attuale). Prima di questa funzione, un
    // confronto esatto delle dimensioni scartava l'intera configurazione
    // salvata (WiFi, matricola, tutto) ad ogni singolo aggiornamento che
    // aggiungeva anche un solo campo - qui invece si copiano solo i byte
    // realmente presenti, lasciando i campi nuovi (in coda alla struct,
    // gia' a apply_defaults() sopra) al loro valore di default. Funziona
    // SOLO se i nuovi campi vengono sempre aggiunti in fondo a
    // app_settings_t (mai inseriti in mezzo): se un campo esistente
    // cambiasse posizione o dimensione, i byte salvati finirebbero nel
    // campo sbagliato - convenzione da rispettare in settings.h.
    size_t stored_size = 0;
    esp_err_t err = nvs_get_blob(h, NVS_KEY_CFG, NULL, &stored_size);
    if (err != ESP_OK || stored_size < sizeof(uint32_t)) {
        nvs_close(h);
        ESP_LOGW(TAG, "Nessuna configurazione salvata in NVS, uso i default di Kconfig");
        return;
    }

    uint8_t *buf = malloc(stored_size);
    if (!buf) {
        nvs_close(h);
        ESP_LOGE(TAG, "Memoria insufficiente per leggere la configurazione NVS, uso i default");
        return;
    }
    err = nvs_get_blob(h, NVS_KEY_CFG, buf, &stored_size);
    nvs_close(h);

    uint32_t magic;
    memcpy(&magic, buf, sizeof(magic));

    if (err == ESP_OK && magic == CFG_MAGIC) {
        size_t settings_bytes = stored_size - sizeof(magic);
        size_t copy_len = settings_bytes < sizeof(app_settings_t) ? settings_bytes : sizeof(app_settings_t);
        memcpy(&s_settings, buf + sizeof(magic), copy_len);
        if (settings_bytes != sizeof(app_settings_t)) {
            ESP_LOGI(TAG, "Configurazione caricata da una versione precedente del firmware (%u/%u byte) - i campi nuovi restano al default finche' non li imposti dalla UI",
                     (unsigned) settings_bytes, (unsigned) sizeof(app_settings_t));
        }
        // Migrazione per dispositivi gia' configurati prima dell'aggiunta
        // del default automatico sopra: una matricola mai impostata
        // resterebbe altrimenti vuota per sempre (il default si applica
        // solo quando non c'e' nessuna configurazione salvata in NVS).
        // Aggiorna anche il formato "MAC per esteso" generato da una
        // primissima versione di questo stesso default (17 caratteri con
        // ':' in quarta posizione, es. "7C:2C:67:89:34:28") al formato
        // breve attuale - riconoscibile perche' nessun utente scriverebbe
        // a mano proprio quel formato, quindi e' sicuramente un valore
        // generato in automatico, non una matricola scelta a mano.
        bool looks_like_old_full_mac = strlen(s_settings.device_serial) == 17 &&
                                        s_settings.device_serial[2] == ':';
        if (s_settings.device_serial[0] == '\0' || looks_like_old_full_mac) {
            format_mac_serial(s_settings.device_serial, sizeof(s_settings.device_serial));
        }
        // Stessa idea per il nome della rete di setup: se risultasse vuoto
        // (configurazione salvata corrotta/incompleta) il dispositivo
        // diventerebbe irraggiungibile in pratica (un AP senza nome e'
        // difficile da trovare per un utente) - si rigenera dal MAC, come
        // gia' fatto in apply_defaults() per un dispositivo mai configurato.
        if (s_settings.ap_ssid[0] == '\0') {
            ESP_LOGW(TAG, "Nome rete AP salvato vuoto, rigenero dal MAC");
            uint8_t mac[6] = {0};
            esp_read_mac(mac, ESP_MAC_WIFI_STA);
            snprintf(s_settings.ap_ssid, sizeof(s_settings.ap_ssid), "EVONETRTK-%02X%02X%02X", mac[3], mac[4], mac[5]);
        }
        if (s_settings.ap_password[0] == '\0') {
            ESP_LOGW(TAG, "Password rete AP salvata vuota, uso il default di fabbrica");
            strncpy(s_settings.ap_password, "baseesp32setup", sizeof(s_settings.ap_password) - 1);
        }
        // Rete di sicurezza indipendente dal magic/dalla dimensione sopra:
        // un valore fuori range qui manderebbe in crash il boot (ESP_ERROR_CHECK
        // dentro uart_driver_install() abortisce su un numero di porta non
        // valido, visto in pratica durante lo sviluppo di questa funzione) -
        // invece di fidarsi ciecamente di un blob NVS che in teoria dovrebbe
        // essere valido ma in pratica (bug, corruzione, versioni future con
        // un bug di migrazione) potrebbe non esserlo, si riporta al default
        // di Kconfig qualunque valore non plausibile.
        if (s_settings.gnss_uart_num < 0 || s_settings.gnss_uart_num > 2) {
            ESP_LOGW(TAG, "gnss_uart_num salvato non valido (%d), uso il default di Kconfig", s_settings.gnss_uart_num);
            s_settings.gnss_uart_num = CONFIG_BASEESP32_GNSS_UART_NUM;
        }
        if (s_settings.gnss_uart_baud < 1200) {
            ESP_LOGW(TAG, "gnss_uart_baud salvato non valido (%d), uso il default di Kconfig", s_settings.gnss_uart_baud);
            s_settings.gnss_uart_baud = CONFIG_BASEESP32_GNSS_UART_BAUD;
        }
        // Rete di sicurezza generale per tutti i campi testo che devono
        // sempre essere ASCII stampabile "normale" (indirizzi, nomi host,
        // matricola...) - a differenza di wifi_ssid/ap_ssid, che possono
        // legittimamente contenere byte non-ASCII (nomi di rete reali) e
        // hanno gia' la loro gestione dedicata (vedi raw_ssid_bytes_to_
        // safe_utf8() in web_ui.c). Trovato dopo che l'utente ha segnalato
        // punti interrogativi nell'indirizzo di aggiornamento online - un
        // campo che l'utente scrive sempre a mano in ASCII puro, quindi
        // qualunque byte "strano" li' dentro e' un segno di configurazione
        // corrotta, non un carattere legittimo. Un campo cosi' viene
        // azzerato (torna a "non configurato") invece di continuare a
        // mostrare byte corrotti per sempre.
        {
            struct { char *field; size_t size; const char *label; } ascii_fields[] = {
                { s_settings.ntrip_host, sizeof(s_settings.ntrip_host), "ntrip_host" },
                { s_settings.ntrip_mountpoint, sizeof(s_settings.ntrip_mountpoint), "ntrip_mountpoint" },
                { s_settings.ntrip_username, sizeof(s_settings.ntrip_username), "ntrip_username" },
                { s_settings.cellular_apn, sizeof(s_settings.cellular_apn), "cellular_apn" },
                { s_settings.ota_update_url, sizeof(s_settings.ota_update_url), "ota_update_url" },
                { s_settings.alert_smtp_host, sizeof(s_settings.alert_smtp_host), "alert_smtp_host" },
                { s_settings.alert_smtp_user, sizeof(s_settings.alert_smtp_user), "alert_smtp_user" },
                { s_settings.alert_email_to, sizeof(s_settings.alert_email_to), "alert_email_to" },
                { s_settings.alert_whatsapp_phone, sizeof(s_settings.alert_whatsapp_phone), "alert_whatsapp_phone" },
                { s_settings.ntrip_caster_server_mountpoint, sizeof(s_settings.ntrip_caster_server_mountpoint), "ntrip_caster_server_mountpoint" },
                { s_settings.ntrip_caster_server_username, sizeof(s_settings.ntrip_caster_server_username), "ntrip_caster_server_username" },
                // device_serial NON e' qui: ha gia' la sua gestione dedicata
                // sopra (rigenera dal MAC se vuota) - includerla anche qui
                // la svuoterebbe DOPO che quel controllo e' gia' passato,
                // senza una seconda occasione di rigenerarla in questo boot.
            };
            for (size_t i = 0; i < sizeof(ascii_fields) / sizeof(ascii_fields[0]); i++) {
                bool clean = true;
                for (size_t j = 0; j < ascii_fields[i].size && ascii_fields[i].field[j] != '\0'; j++) {
                    unsigned char c = (unsigned char) ascii_fields[i].field[j];
                    if (c < 0x20 || c > 0x7E) {
                        clean = false;
                        break;
                    }
                }
                if (!clean) {
                    ESP_LOGW(TAG, "Campo '%s' salvato con byte non validi, azzerato", ascii_fields[i].label);
                    ascii_fields[i].field[0] = '\0';
                }
            }
        }
        ESP_LOGI(TAG, "Configurazione caricata da NVS (AP=%s)", s_settings.ap_ssid);
    } else {
        ESP_LOGW(TAG, "Configurazione NVS non valida, uso i default di Kconfig");
    }
    free(buf);
}

app_settings_t settings_get(void)
{
    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    app_settings_t copy = s_settings;
    xSemaphoreGive(s_settings_mutex);
    return copy;
}

esp_err_t settings_save(const app_settings_t *s)
{
    // Diagnostica per capire se un valore gia' scorretto arriva fin qui
    // (bug a monte, nella UI/nel parsing del form) o se si corrompe dopo
    // (bug nel giro di scrittura/lettura NVS) - lasciato attivo in modo
    // permanente, costa una sola riga di log per ogni salvataggio (azione
    // rara, non nel percorso critico).
    ESP_LOGI(TAG, "Salvataggio impostazioni: wifi_ssid='%s' (len %d) ap_ssid='%s' gnss_uart_num=%d gnss_uart_baud=%d sizeof=%u",
             s->wifi_ssid, (int) strlen(s->wifi_ssid), s->ap_ssid, s->gnss_uart_num, s->gnss_uart_baud,
             (unsigned) sizeof(*s));

    xSemaphoreTake(s_settings_mutex, portMAX_DELAY);
    s_settings = *s;
    xSemaphoreGive(s_settings_mutex);

    // Costruito da *s (parametro del chiamante, non condiviso/soggetto a
    // scritture concorrenti) e non da s_settings: evita qualunque finestra
    // di rischio tra il rilascio del mutex sopra e questa riga.
    stored_cfg_t stored = { .magic = CFG_MAGIC, .s = *s };

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY_CFG, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    ESP_LOGI(TAG, "Scrittura NVS completata: %s (%u byte scritti)", esp_err_to_name(err), (unsigned) sizeof(stored));

    // Rilettura immediata di verifica (stessa diagnostica di sopra) -
    // conferma se quanto e' stato appena scritto combacia con quanto si
    // rilegge subito dopo, per escludere un problema nel giro di
    // scrittura/lettura NVS invece che nei dati arrivati a questa funzione.
    nvs_handle_t hv;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &hv) == ESP_OK) {
        stored_cfg_t verify;
        size_t len = sizeof(verify);
        if (nvs_get_blob(hv, NVS_KEY_CFG, &verify, &len) == ESP_OK) {
            ESP_LOGI(TAG, "Verifica rilettura: %u byte, wifi_ssid='%s' ap_ssid='%s' gnss_uart_num=%d gnss_uart_baud=%d",
                     (unsigned) len, verify.s.wifi_ssid, verify.s.ap_ssid, verify.s.gnss_uart_num, verify.s.gnss_uart_baud);
        } else {
            ESP_LOGW(TAG, "Verifica rilettura fallita");
        }
        nvs_close(hv);
    }

    return err;
}

void app_settings_remember_wifi(app_settings_t *s, const char *ssid, const char *password)
{
    if (strcmp(s->wifi_ssid, ssid) == 0) {
        // Stessa rete di prima (solo la password puo' essere cambiata):
        // resta la principale, nessun elenco da toccare.
        strncpy(s->wifi_password, password, sizeof(s->wifi_password) - 1);
        s->wifi_password[sizeof(s->wifi_password) - 1] = '\0';
        return;
    }

    // Se la rete richiesta era gia' tra le "conosciute", toglila da li':
    // sta per tornare ad essere la principale, non deve comparire due volte.
    for (int i = 0; i < WIFI_KNOWN_NETWORKS_MAX; i++) {
        if (strcmp(s->wifi_known_networks[i].ssid, ssid) == 0) {
            for (int j = i; j < WIFI_KNOWN_NETWORKS_MAX - 1; j++) {
                s->wifi_known_networks[j] = s->wifi_known_networks[j + 1];
            }
            memset(&s->wifi_known_networks[WIFI_KNOWN_NETWORKS_MAX - 1], 0,
                   sizeof(s->wifi_known_networks[0]));
            break;
        }
    }

    // La vecchia principale (se impostata) scende in cima all'elenco delle
    // conosciute - le altre si spostano di una posizione, la piu' vecchia
    // in fondo esce se l'elenco e' gia' pieno.
    if (s->wifi_ssid[0] != '\0') {
        for (int i = WIFI_KNOWN_NETWORKS_MAX - 1; i > 0; i--) {
            s->wifi_known_networks[i] = s->wifi_known_networks[i - 1];
        }
        strncpy(s->wifi_known_networks[0].ssid, s->wifi_ssid, sizeof(s->wifi_known_networks[0].ssid) - 1);
        s->wifi_known_networks[0].ssid[sizeof(s->wifi_known_networks[0].ssid) - 1] = '\0';
        strncpy(s->wifi_known_networks[0].password, s->wifi_password, sizeof(s->wifi_known_networks[0].password) - 1);
        s->wifi_known_networks[0].password[sizeof(s->wifi_known_networks[0].password) - 1] = '\0';
    }

    strncpy(s->wifi_ssid, ssid, sizeof(s->wifi_ssid) - 1);
    s->wifi_ssid[sizeof(s->wifi_ssid) - 1] = '\0';
    strncpy(s->wifi_password, password, sizeof(s->wifi_password) - 1);
    s->wifi_password[sizeof(s->wifi_password) - 1] = '\0';
}
