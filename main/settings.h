#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum {
    GNSS_CHIP_UBLOX = 0,
    GNSS_CHIP_UNICORE = 1,
    GNSS_CHIP_LC29H = 2, // Quectel LC29H (BA/CA/DA/EA), comandi $PQTM.../$PAIR...
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

typedef enum {
    OLED_CTRL_SSD1306 = 0,
    OLED_CTRL_SH1106 = 1,
    OLED_CTRL_SSD1309 = 2, // stesso percorso di SSD1306 nel driver, vedi commento sul campo sotto
} oled_controller_t;

typedef enum {
    BASE_POSITION_AUTO = 0,   // survey-in ad ogni avvio (comportamento storico, precisione tipicamente metrica)
    BASE_POSITION_MANUAL = 1, // coordinate fisse note (es. da un servizio di post-processing PPP)
} base_position_mode_t;

// Reti WiFi "conosciute": ogni volta che una connessione tramite "Connetti"
// (vedi web_ui.c) riesce davvero, quella rete viene ricordata qui (vedi
// app_settings_remember_wifi() sotto) - il dispositivo puo' cosi' spostarsi
// tra piu' reti gia' provate in passato (es. hotspot del campo, WiFi di
// casa) riconnettendosi da solo a quella visibile, senza dover reinserire
// SSID/password ogni volta. wifi_ssid/wifi_password restano la rete
// "principale" (provata per prima); questo elenco tiene le altre.
#define WIFI_KNOWN_NETWORKS_MAX 5

typedef struct {
    char ssid[33];
    char password[65];
} wifi_known_network_t;

// Livello dei messaggi RTCM3 MSM (Multiple Signal Message) da inviare per
// ciascuna costellazione quando il dispositivo e' base - richiesto
// dall'utente per poter ridurre il volume dati/messaggi su ricevitori
// rover con memoria limitata, invece di avere un set fisso di messaggi
// deciso dal firmware. MSM4 = risoluzione standard (meno dati), MSM7 =
// massima risoluzione (piu' dati). Non tutti i chip GNSS supportati
// permettono lo stesso livello di controllo: u-blox e Unicore possono
// scegliere per singola costellazione, il Quectel LC29H (vedi
// gnss_lc29h.c) ha solo un interruttore globale MSM4/MSM7 lato modulo -
// li' il livello piu' alto richiesto tra le costellazioni abilitate vale
// per tutte.
typedef enum {
    RTCM_MSM_OFF = 0,
    RTCM_MSM4 = 1,
    RTCM_MSM7 = 2,
} rtcm_msm_level_t;

// IMPORTANTE per chi modifica questa struct: settings.c salva/carica questi
// campi come blob grezzo in NVS, con una migrazione che permette di
// aggiungere nuovi campi senza perdere la configurazione gia' salvata dagli
// utenti (WiFi, matricola, ecc.) - ma funziona SOLO se ogni nuovo campo
// viene aggiunto IN FONDO alla struct, mai inserito in mezzo o tolto/
// rinominato: altrimenti i byte salvati per i campi esistenti finirebbero
// interpretati come il campo sbagliato. Vedi settings_init() in settings.c.
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
    // default lo stesso suffisso (3 byte del MAC di fabbrica) gia' usato
    // per l'SSID dell'AP di setup (es. "EVONETRTK-893428" -> "893428"),
    // sovrascrivibile dalla UI web con un proprio codice se lo si preferisce.
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
    // dimensione fisica). SSD1309 (tipico sui moduli piu' grandi, 2,42")
    // e' invece compatibile a livello di comandi con SSD1306 nella
    // stragrande maggioranza dei moduli in commercio - stesso percorso nel
    // driver, tenuto come opzione separata solo per mostrare all'utente il
    // controller giusto invece di uno diverso ma elettricamente
    // equivalente.
    oled_controller_t oled_controller;
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

    // Avviso (email e/o WhatsApp) quando la connessione al caster NTRIP
    // resta giu' oltre alert_threshold_min minuti - pensato per una base
    // lasciata incustodita in campo, dove nessuno se ne accorgerebbe
    // altrimenti finche' non si notano problemi sul rover. Un canale resta
    // disattivato se i suoi campi sono vuoti (basta configurarne uno solo,
    // o entrambi).
    bool alert_enable;
    uint16_t alert_threshold_min;   // minuti di disconnessione prima di avvisare
    char alert_smtp_host[64];
    uint16_t alert_smtp_port;       // 465 = SMTPS (TLS implicito), tipico per invio autenticato
    char alert_smtp_user[64];       // anche mittente ("From") e utente per l'autenticazione
    char alert_smtp_password[64];
    char alert_email_to[64];
    char alert_whatsapp_phone[24];  // con prefisso internazionale, es. "391234567890" (CallMeBot)
    char alert_whatsapp_apikey[16];

    // Avviso (stessi canali sopra) se l'antenna della base si sposta
    // rispetto alla posizione registrata al primo frame RTCM 1005/1006
    // ricevuto dopo l'avvio (es. urtata da un mezzo agricolo o dal vento) -
    // uno spostamento non rilevato fa arrivare correzioni sbagliate a tutti
    // i rover collegati, senza nessun segnale visibile sul posto. Attivo
    // solo se alert_enable e' true, in aggiunta ad esso.
    bool base_drift_alert_enable;
    float base_drift_threshold_m; // distanza minima per considerarlo uno spostamento reale, non rumore di misura

    // Server caster NTRIP locale (solo modalita' base): oltre a inoltrare
    // l'RTCM3 al caster esterno configurato sopra, il dispositivo puo'
    // anche accettare direttamente connessioni da rover (protocollo NTRIP
    // server-side) - utile in campo senza internet, dove base e rover si
    // scambiano le correzioni sulla stessa rete WiFi locale senza bisogno
    // di un caster esterno. Raggiungibile da internet solo se l'utente
    // configura il port forwarding sul proprio router (impossibile in
    // pratica sui dati del modem cellulare, quasi sempre dietro NAT
    // condiviso dall'operatore) - vedi hint nella UI web.
    bool ntrip_caster_server_enable;
    uint16_t ntrip_caster_server_port; // 2101 = porta standard NTRIP
    char ntrip_caster_server_mountpoint[33];
    char ntrip_caster_server_username[33]; // vuoto = nessuna autenticazione richiesta
    char ntrip_caster_server_password[64];

    // Posizione dell'antenna in modalita' base (solo effettivo sul chip
    // LC29H per ora, vedi gnss_lc29h.c). "Automatica" ripete il survey-in
    // (media pesata di qualche minuto, precisione tipicamente metrica) ad
    // ogni avvio, come sempre fatto finora. "Manuale" usa queste coordinate
    // fisse - tipicamente ottenute da un servizio di post-processing PPP
    // (es. CSRS-PPP, OPUS: si registrano le osservazioni grezze per molte
    // ore e si caricano sul sito, che restituisce una posizione accurata a
    // livello di centimetri, molto piu' precisa del solo survey-in). La UI
    // web propone come default gli stessi valori dell'ultima posizione
    // rilevata dal ricevitore (vedi base_monitor.h), cosi' di norma basta
    // confermare invece di doverli scrivere a mano.
    base_position_mode_t base_position_mode;
    double base_fixed_lat_deg;
    double base_fixed_lon_deg;
    double base_fixed_height_m; // quota ellissoidica WGS84, non sul livello del mare

    // Controllo/applicazione automatica degli aggiornamenti online (vedi
    // auto_update.c) - disattivato di default: e' una scelta esplicita
    // dell'utente, non il comportamento predefinito, perche' comporta
    // comunque un riavvio non presidiato del dispositivo. Utile soprattutto
    // quando il dispositivo e' raggiungibile solo via cellulare (SIM7600/
    // SIM868): il controllo/download e' un collegamento in USCITA verso
    // GitHub, funziona anche dietro il NAT condiviso degli operatori mobili
    // che invece impedisce di raggiungere la pagina web da remoto.
    bool auto_update_check_enable;
    uint16_t auto_update_check_interval_h; // ore tra un controllo e l'altro

    wifi_known_network_t wifi_known_networks[WIFI_KNOWN_NETWORKS_MAX];

    // Selezione messaggi RTCM3 per la base (vedi rtcm_msm_level_t sopra) -
    // il default (impostato in apply_defaults()) riproduce il comportamento
    // storico gia' in uso (MSM7 su tutte le costellazioni + 1005 + 1230),
    // cosi' un dispositivo gia' in campo non cambia comportamento finche'
    // non si tocca esplicitamente questa impostazione.
    rtcm_msm_level_t rtcm_gps_msm;
    rtcm_msm_level_t rtcm_glonass_msm;
    rtcm_msm_level_t rtcm_galileo_msm;
    rtcm_msm_level_t rtcm_beidou_msm;
    bool rtcm_1005_enable; // posizione base (senza posizione RTK non ha senso disattivarlo, ma resta scelta esplicita dell'utente)
    bool rtcm_1230_enable; // bias di fase/codice GLONASS
} app_settings_t;

// Segna ssid/password come rete WiFi funzionante (verificata, non solo
// digitata): *s (non salvato da solo, il chiamante decide quando/se
// persistere con settings_save()) viene aggiornato cosi' che quella rete
// diventi la "principale" (wifi_ssid/wifi_password, provata per prima ad
// ogni riconnessione); se stava sostituendo una rete diversa gia'
// impostata, quella precedente entra in testa all'elenco delle reti
// "conosciute" sopra (le piu' vecchie escono oltre WIFI_KNOWN_NETWORKS_MAX
// voci). Da chiamare SOLO dopo una connessione riuscita per davvero (mai
// su un semplice salvataggio dal form, che non verifica la connettivita'),
// cosi' l'elenco contiene solo reti gia' provate con successo.
void app_settings_remember_wifi(app_settings_t *s, const char *ssid, const char *password);

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

// Formatta in "out" la matricola che si otterrebbe dal MAC WiFi del chip
// (stessa logica usata per il default in apply_defaults()) - serve alla UI
// web per segnalare quando settings.device_serial e' stato cambiato a mano
// e non corrisponde piu' al chip fisico su cui gira il firmware.
void settings_device_serial_from_mac(char *out, size_t out_size);
