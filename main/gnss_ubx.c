#include "gnss_ubx.h"
#include "gnss_io.h"
#include "settings.h"
#include "gnss_ubx_ack.h"
#include "status.h"
#include "esp_timer.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

    // Un unico buffer e un'unica gnss_io_write(), non piu' tre chiamate
    // separate (header, poi payload, poi checksum) - un piccolo ritardo di
    // scheduling FreeRTOS tra quelle chiamate poteva far vedere al
    // ricevitore un frame "a pezzi" se ha un timeout stretto tra un byte e
    // l'altro, spiegando potenzialmente i NAK visti su ogni singolo
    // comando mandato finora, incluso un poll MON-VER a payload vuoto che
    // via USB diretto dallo stesso PC (senza questa frammentazione)
    // funziona invece perfettamente - da verificare con questo fix.
    uint8_t frame[6 + 512 + 2];
    frame[0] = UBX_SYNC1;
    frame[1] = UBX_SYNC2;
    frame[2] = msg_class;
    frame[3] = msg_id;
    frame[4] = (uint8_t) (payload_len & 0xFF);
    frame[5] = (uint8_t) (payload_len >> 8);
    if (payload_len > 0) {
        memcpy(frame + 6, payload, payload_len);
    }

    uint8_t ck_a, ck_b;
    ubx_checksum(frame + 2, 4 + payload_len, &ck_a, &ck_b);
    frame[6 + payload_len] = ck_a;
    frame[6 + payload_len + 1] = ck_b;

    // Ogni comando mandato qui attende una risposta (ACK/NAK o MON-VER):
    // finestra aperta prima di scrivere, cosi' anche una risposta
    // velocissima trova il parser attivo e la coda gia' svuotata.
    gnss_ubx_ack_arm();
    gnss_io_write((const char *) frame, 6 + payload_len + 2);
    return ESP_OK;
}

// Coppia chiave/valore UBX-CFG-VALSET. Il valore e' tenuto qui a 32 bit per
// comodita' del chiamante, ma sul filo va scritto con un numero di byte
// diverso a seconda della chiave (vedi ubx_key_value_size sotto) - NON
// sempre 4, come invece faceva la versione precedente di questo file.
typedef struct __attribute__((packed)) {
    uint32_t key;
    uint32_t value;
} ubx_cfg_kv32_t;

// La classe dimensione del valore e' codificata nei bit 28-30 della chiave
// stessa (u-blox interface manual, formato "Key ID"): 1=1 bit (un byte sul
// filo), 2=1 byte, 3=2 byte, 4=4 byte, 5=8 byte. Il protocollo CFG-VALSET
// impacchetta ogni valore con ESATTAMENTE questo numero di byte, senza
// padding - la maggior parte delle chiavi enable/disable usate in questo
// file (es. tutte le CFG-MSGOUT-*, CFG-UART1*PROT-*, CFG-TMODE-MODE) sono
// da 1 byte. Scrivere sempre un uint32_t come faceva il codice precedente
// disallineava ogni chiave successiva nello stesso messaggio, facendola
// interpretare come spazzatura dal ricevitore - bug preesistente, mai
// notato perche' fallisce in silenzio (nessun errore, la chiave semplice-
// mente non si applica). Confermato contro PX4-GPSDrivers (driver di
// produzione, usa cfgValset<uint8_t> per le stesse chiavi).
static size_t ubx_key_value_size(uint32_t key)
{
    switch ((key >> 28) & 0x7) {
        case 1: return 1;
        case 2: return 1;
        case 3: return 2;
        case 4: return 4;
        case 5: return 8; // nessuna chiave da 8 byte usata qui: il campo value a 32 bit non basterebbe
        default: return 4;
    }
}

static esp_err_t ubx_valset(uart_port_t uart_num, const char *label, const ubx_cfg_kv32_t *kvs, size_t n)
{
    if (n > 32) {
        return ESP_ERR_INVALID_SIZE;
    }

    uint8_t payload[4 + 32 * (4 + 4)];
    size_t off = 4;
    payload[0] = 0x00; // version
    payload[1] = 0x01; // layer: RAM (applicazione immediata, non persistente su BBR/flash)
    payload[2] = 0x00; // reserved
    payload[3] = 0x00; // reserved

    for (size_t i = 0; i < n; i++) {
        memcpy(payload + off, &kvs[i].key, 4);
        off += 4;
        size_t vsize = ubx_key_value_size(kvs[i].key);
        if (vsize > sizeof(kvs[i].value)) {
            vsize = sizeof(kvs[i].value); // difensivo: mai il caso oggi (nessuna chiave da 8 byte)
        }
        memcpy(payload + off, &kvs[i].value, vsize); // little-endian: i byte bassi sono quelli giusti
        off += vsize;
    }

    // UBX-CFG-VALSET = classe 0x06, id 0x8A
    // Verifica reale (non piu' "spara e spera"): il ricevitore risponde
    // sempre con UBX-ACK-ACK o UBX-ACK-NAK a un CFG-VALSET. Il timeout e'
    // generoso perche' questi comandi girano a bassa priorita' rispetto al
    // flusso NMEA/RTCM in tempo reale sulla stessa UART.
    // Senza nessuna risposta si riprova: con il ricevitore via I2C i primi
    // secondi dopo un riavvio dell'ESP32 (con il ricevitore rimasto acceso)
    // possono dare errori sul bus, e una configurazione persa li' resterebbe
    // persa fino al riavvio successivo. Un NAK invece non si riprova: e' un
    // rifiuto vero, riprovare darebbe lo stesso risultato.
    bool acked = false;
    uint8_t echo_cls = 0, echo_id = 0;
    bool answered = false;
    for (int attempt = 1; attempt <= 3 && !answered; attempt++) {
        esp_err_t err = ubx_send(uart_num, 0x06, 0x8A, payload, (uint16_t) off);
        if (err != ESP_OK) {
            return err;
        }
        answered = gnss_ubx_ack_wait(500, &acked, &echo_cls, &echo_id);
        if (!answered && attempt < 3) {
            ESP_LOGW(TAG, "[%s] Nessuna risposta dal ricevitore, riprovo (%d/3)", label, attempt + 1);
            vTaskDelay(pdMS_TO_TICKS(1000));
        }
    }
    if (!answered) {
        ESP_LOGW(TAG, "[%s] Nessuna risposta ACK/NAK dal ricevitore dopo 3 tentativi (collegamento sbagliato? "
                       "ricevitore che non implementa CFG-VALSET?)", label);
    } else if (!acked) {
        ESP_LOGE(TAG, "[%s] Ricevitore ha rifiutato la configurazione (UBX-ACK-NAK, riferita a classe "
                       "0x%02X id 0x%02X - mandato 0x06 0x8A) - una o piu' chiavi in questo gruppo non "
                       "sono valide per questo modulo, oppure la risposta non si riferisce affatto a "
                       "questo comando se le classi non combaciano", label, echo_cls, echo_id);
    } else {
        ESP_LOGI(TAG, "[%s] Configurazione accettata dal ricevitore (UBX-ACK-ACK)", label);
    }

    return ESP_OK;
}

// Chiavi CFG-MSGOUT-RTCM_3X_TYPE*_UART1, verificate contro
// sparkfun/SparkFun_u-blox_GNSS_Arduino_Library (src/u-blox_config_keys.h,
// libreria di terzi ampiamente usata, non la fonte ufficiale u-blox diretta
// ma un riscontro indipendente) dopo aver scoperto che i valori precedenti
// in questo file erano sbagliati (quasi tutti spostati di una posizione:
// es. 1005 era 0x209102bd invece di 0x209102be) - la configurazione RTCM3
// della base molto probabilmente non ha mai funzionato correttamente su
// hardware u-blox reale prima di questo fix, dato che una chiave sbagliata
// viene rifiutata in silenzio (vedi avvertenza in gnss_ubx.h). u-blox NON
// supporta affatto 1007/1008/1019/1020 in uscita (nessuna chiave
// corrispondente esiste in quella libreria): se richiesti dalle
// impostazioni vengono ignorati con un log, non c'e' modo di inviarli.
static esp_err_t ubx_valset_group(uart_port_t uart_num, const char *group_name,
                                   const ubx_cfg_kv32_t *kvs, size_t n);
static void svin_poll_task(void *arg);

esp_err_t gnss_ubx_configure_base(uart_port_t uart_num)
{
    bool i2c = gnss_io_is_i2c();
    ESP_LOGI(TAG, "Configuro ricevitore u-blox come base RTK (Survey-In + RTCM3 su %s)", i2c ? "I2C" : "UART1");

    app_settings_t s; // riempita senza copie temporanee (vedi settings_get_into)
    settings_get_into(&s);

    if (s.rtcm_1007_enable || s.rtcm_1008_enable || s.rtcm_1019_enable || s.rtcm_1020_enable) {
        ESP_LOGW(TAG, "1007/1008/1019/1020 richiesti nelle impostazioni ma non supportati da u-blox in uscita: ignorati");
    }

    gnss_ubx_poll_version(uart_num);
    vTaskDelay(pdMS_TO_TICKS(300)); // tempo per la risposta, loggata in modo asincrono

    // Ogni chiave CFG-MSGOUT esiste per porta, in ordine I2C, UART1, UART2,
    // USB, SPI: la variante _I2C vale quella _UART1 meno 1.
    const uint32_t port_off = i2c ? 1 : 0;

    // Gruppi separati come nel rover: VALSET e' tutto-o-niente, un NAK su
    // un gruppo non deve far cadere anche gli altri, e il log dice quale.
    const ubx_cfg_kv32_t kvs_port[] = {
        // CFG-I2COUTPROT / CFG-UART1OUTPROT: RTCM3 in uscita, e anche NMEA:
        // GGA/GSV servono a stato fix e grafico satelliti della base.
        // base_stream_demux.c separa i flussi, al caster va solo l'RTCM3.
        { i2c ? 0x10720004 : 0x10740004, 1 },
        { i2c ? 0x10720002 : 0x10740002, 1 },
        { 0x209100bb - port_off, 1 },   // CFG-MSGOUT-NMEA_ID_GGA (ogni epoca)
        { 0x209100c5 - port_off, 1 },   // CFG-MSGOUT-NMEA_ID_GSV
    };
    ubx_valset_group(uart_num, i2c ? "PORTA I2C" : "PORTA UART1", kvs_port, sizeof(kvs_port) / sizeof(kvs_port[0]));

    ubx_cfg_kv32_t kvs[16];
    size_t n = 0;
    kvs[n++] = (ubx_cfg_kv32_t) { 0x209102be - port_off, s.rtcm_1005_enable ? 10u : 0u }; // CFG-MSGOUT-RTCM_3X_TYPE1005 (posizione base), ogni 10 epoche: non cambia, ogni secondo era traffico sprecato
    kvs[n++] = (ubx_cfg_kv32_t) { 0x20910304 - port_off, s.rtcm_1230_enable ? 1u : 0u }; // TYPE1230 (bias GLONASS)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x2091035f - port_off, s.rtcm_1074_enable ? 1u : 0u }; // TYPE1074 (GPS MSM4)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x209102cd - port_off, s.rtcm_1077_enable ? 1u : 0u }; // TYPE1077 (GPS MSM7)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x20910364 - port_off, s.rtcm_1084_enable ? 1u : 0u }; // TYPE1084 (GLONASS MSM4)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x209102d2 - port_off, s.rtcm_1087_enable ? 1u : 0u }; // TYPE1087 (GLONASS MSM7)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x20910369 - port_off, s.rtcm_1094_enable ? 1u : 0u }; // TYPE1094 (Galileo MSM4)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x20910319 - port_off, s.rtcm_1097_enable ? 1u : 0u }; // TYPE1097 (Galileo MSM7)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x2091036e - port_off, s.rtcm_1124_enable ? 1u : 0u }; // TYPE1124 (BeiDou MSM4)
    kvs[n++] = (ubx_cfg_kv32_t) { 0x209102d7 - port_off, s.rtcm_1127_enable ? 1u : 0u }; // TYPE1127 (BeiDou MSM7)
    ubx_valset_group(uart_num, "MESSAGGI RTCM", kvs, n);

    if (s.base_position_mode == BASE_POSITION_MANUAL) {
        // Posizione fissa nota (CFG-TMODE-MODE = 2, coordinate LLH): niente
        // survey-in, la base manda subito il 1005 con queste coordinate.
        // Prima questo ramo mancava e con un u-blox le coordinate inserite
        // nel pannello venivano ignorate senza avviso (valeva solo per LC29H).
        // Lat/lon in 1e-7 gradi + parte fine in 1e-9; quota in cm + parte fine
        // in 0,1 mm (formato richiesto dal ricevitore, interface description).
        int64_t lat_e9 = llround(s.base_fixed_lat_deg * 1e9);
        int64_t lon_e9 = llround(s.base_fixed_lon_deg * 1e9);
        int64_t h_01mm = llround(s.base_fixed_height_m * 1e4);
        int32_t lat = (int32_t) (lat_e9 / 100), lat_hp = (int32_t) (lat_e9 % 100);
        int32_t lon = (int32_t) (lon_e9 / 100), lon_hp = (int32_t) (lon_e9 % 100);
        int32_t h_cm = (int32_t) (h_01mm / 100), h_hp = (int32_t) (h_01mm % 100);
        ESP_LOGI(TAG, "Posizione base fissa: lat %.9f lon %.9f quota ellissoidica %.4f m",
                 s.base_fixed_lat_deg, s.base_fixed_lon_deg, s.base_fixed_height_m);
        const ubx_cfg_kv32_t kvs_fixed[] = {
            { 0x20030001, 2 },                 // CFG-TMODE-MODE = 2 (posizione fissa)
            { 0x20030002, 1 },                 // CFG-TMODE-POS_TYPE = 1 (LLH)
            { 0x40030009, (uint32_t) lat },    // CFG-TMODE-LAT (1e-7 gradi)
            { 0x2003000c, (uint32_t) lat_hp }, // CFG-TMODE-LAT_HP (1e-9 gradi)
            { 0x4003000a, (uint32_t) lon },    // CFG-TMODE-LON
            { 0x2003000d, (uint32_t) lon_hp }, // CFG-TMODE-LON_HP
            { 0x4003000b, (uint32_t) h_cm },   // CFG-TMODE-HEIGHT (cm)
            { 0x2003000e, (uint32_t) h_hp },   // CFG-TMODE-HEIGHT_HP (0,1 mm)
            { 0x4003000f, 100 },               // CFG-TMODE-FIXED_POS_ACC: 10 mm (0,1 mm)
        };
        ubx_valset_group(uart_num, "TMODE (posizione fissa)", kvs_fixed, sizeof(kvs_fixed) / sizeof(kvs_fixed[0]));
        return ESP_OK;
    }

    // Durata e precisione dal pannello (0 = predefinito: 60 s, 0,25 m).
    uint32_t svin_dur_s = s.base_svin_min_dur_s ? s.base_svin_min_dur_s : 60;
    uint32_t svin_acc_01mm = (s.base_svin_acc_m > 0) ? (uint32_t) (s.base_svin_acc_m * 10000.0f + 0.5f) : 2500;
    ESP_LOGI(TAG, "Survey-in: almeno %u s, precisione %.2f m", (unsigned) svin_dur_s, svin_acc_01mm / 10000.0);
    const ubx_cfg_kv32_t kvs_tmode[] = {
        { 0x20030001, 1 },              // CFG-TMODE-MODE = 1 (Survey-In)
        { 0x40030010, svin_dur_s },     // CFG-TMODE-SVIN-MIN-DUR: durata minima survey-in (s) - chiave corretta, era scambiata con quella sotto
        { 0x40030011, svin_acc_01mm },  // CFG-TMODE-SVIN-ACC-LIMIT: precisione richiesta, unita' 0.1mm (2500 = 250mm) - chiave corretta, era scambiata con quella sopra
    };
    ubx_valset_group(uart_num, "TMODE (survey-in)", kvs_tmode, sizeof(kvs_tmode) / sizeof(kvs_tmode[0]));

    // Stato del survey-in per log e pannello (vedi svin_poll_task).
    xTaskCreate(svin_poll_task, "ubx_svin", 3072, (void *) (intptr_t) uart_num, 3, NULL);

    return ESP_OK;
}

// Manda un gruppo di chiavi come invio CFG-VALSET a se stante, loggando
// quale gruppo (per nome) viene eventualmente rifiutato - un solo grosso
// invio con chiavi eterogenee nasconde quale parte specifica fallisce,
// perche' VALSET e' tutto-o-niente: un NAK fa cadere anche le chiavi
// valide spedite nello stesso messaggio (scoperto in pratica su questo
// modulo: l'invio unico di prima veniva sempre rifiutato nel suo
// complesso, senza modo di sapere quale gruppo fosse il colpevole).
static esp_err_t ubx_valset_group(uart_port_t uart_num, const char *group_name,
                                   const ubx_cfg_kv32_t *kvs, size_t n)
{
    esp_err_t err = ubx_valset(uart_num, group_name, kvs, n);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "[%s] invio fallito verso il ricevitore", group_name);
    }
    return err;
}

// Ogni 10 s chiede al ricevitore lo stato del survey-in (UBX-NAV-SVIN, la
// risposta la legge gnss_ubx_ack.c e la mette in status.c per il pannello).
// Prima non si sapeva se il survey-in stesse andando avanti: una base senza
// survey-in completato non manda mai 1005 (la sua posizione) e i rover non
// possono usarla, senza nessun segno visibile.
static void svin_poll_task(void *arg)
{
    uart_port_t uart_num = (uart_port_t) (intptr_t) arg;
    bool logged_valid = false;
    int64_t last_log_us = 0;
    int loop_count = 0;
    while (1) {
        if (ubx_send(uart_num, 0x01, 0x3B, NULL, 0) == ESP_OK) {
            // Nessun ACK per un poll: l'attesa serve solo a tenere aperta la
            // finestra di ascolto (vedi gnss_ubx_ack.h) finche' arriva la
            // risposta. I messaggi NAV polled partono alla soluzione di
            // navigazione successiva, fino a 1 s dopo (con 300 ms, come per
            // MON-VER, la risposta non arrivava mai in tempo).
            gnss_ubx_ack_wait(1200, NULL, NULL, NULL);
        }
        // Ogni minuto anche UBX-MON-SYS: temperatura e stato del ricevitore
        // (lo ZED-F9P HPG 1.51 risponde, verificato il 03/10/2026).
        if (loop_count++ % 6 == 0 && ubx_send(uart_num, 0x0A, 0x39, NULL, 0) == ESP_OK) {
            gnss_ubx_ack_wait(1200, NULL, NULL, NULL);
        }
        // Modello e firmware del ricevitore (scheda di accesso, pannello): la
        // risposta a MON-VER dell'avvio si perde se lo ZED, rimasto acceso
        // durante il riavvio dell'ESP32, ha gia' dati in coda. Si richiede
        // ogni minuto finche' non arriva.
        if (loop_count % 6 == 3 && gnss_ubx_ack_model()[0] == 0 &&
            ubx_send(uart_num, 0x0A, 0x04, NULL, 0) == ESP_OK) {
            gnss_ubx_ack_wait(1200, NULL, NULL, NULL);
        }
        svin_status_t sv = status_svin_get();
        int64_t now = esp_timer_get_time();
        if (sv.have && sv.valid && !logged_valid) {
            ESP_LOGI(TAG, "Survey-in completato: %u s, precisione %.2f m, %u osservazioni - la base manda la sua posizione (1005)",
                     (unsigned) sv.duration_s, sv.mean_acc_m, (unsigned) sv.observations);
            logged_valid = true;
        } else if (sv.have && !sv.valid && now - last_log_us > 5LL * 60 * 1000000) {
            ESP_LOGI(TAG, "Survey-in %s: %u s, precisione attuale %.2f m",
                     sv.active ? "in corso" : "non attivo", (unsigned) sv.duration_s, sv.mean_acc_m);
            last_log_us = now;
        }
        vTaskDelay(pdMS_TO_TICKS(10000));
    }
}

void gnss_ubx_hw_reset(void)
{
    // UBX-CFG-RST (0x06 0x04): navBbrMask 0x0000 = avvio a caldo,
    // resetMode 0x00 = riavvio hardware immediato (watchdog). Nessun ACK.
    uint8_t f[6 + 4 + 2] = { UBX_SYNC1, UBX_SYNC2, 0x06, 0x04, 4, 0, 0x00, 0x00, 0x00, 0x00 };
    ubx_checksum(f + 2, 4 + 4, &f[10], &f[11]);
    gnss_io_write(f, sizeof(f));
    vTaskDelay(pdMS_TO_TICKS(500)); // la scrittura I2C la fa il suo task, entro ~20 ms
    ESP_LOGW(TAG, "Inviato riavvio hardware al ricevitore u-blox (UBX-CFG-RST)");
}

esp_err_t gnss_ubx_poll_version(uart_port_t uart_num)
{
    // UBX-MON-VER = classe 0x0A, id 0x04, nessun payload per il poll. La
    // risposta vera arriva come messaggio a se stante (loggata da
    // gnss_ubx_ack.c), ma se il ricevitore non la riconosce puo' rispondere
    // con un ACK-NAK invece - controllato qui esplicitamente, altrimenti la
    // gnss_ubx_ack_arm() del prossimo ubx_valset_group() lo scarterebbe in
    // silenzio senza che lo si sappia mai (distingue "nessuna risposta
    // affatto" da "rifiutata esplicitamente").
    esp_err_t err = ubx_send(uart_num, 0x0A, 0x04, NULL, 0);
    if (err != ESP_OK) {
        return err;
    }
    bool acked = false;
    uint8_t echo_cls = 0, echo_id = 0;
    if (!gnss_ubx_ack_wait(300, &acked, &echo_cls, &echo_id)) {
        // Normale: un ricevitore che risponde a MON-VER manda la versione,
        // non un ACK. Il problema c'e' solo se manca anche la riga
        // "UBX-MON-VER: swVersion=..." di gnss_ubx_ack.c.
        ESP_LOGI(TAG, "MON-VER: nessun ACK/NAK (normale). Se sopra manca la riga \"UBX-MON-VER: swVersion=...\", "
                      "il ricevitore non risponde affatto");
    } else if (!acked) {
        ESP_LOGW(TAG, "MON-VER: rifiutato con ACK-NAK riferito a classe 0x%02X id 0x%02X (mandato 0x0A 0x04)",
                  echo_cls, echo_id);
    }
    // Se acked==true qui sarebbe un errore di logica: MON-VER non e' un
    // comando "set", non dovrebbe mai generare un ACK-ACK vero e proprio -
    // in quel caso il log della vera risposta (se arrivata) resta comunque
    // quello scritto da gnss_ubx_ack.c per class=0x0A id=0x04.
    return ESP_OK;
}

static void peek_measure_active(const app_settings_t *s, void *ctx)
{
    *(bool *) ctx = s->base_measure_active;
}

esp_err_t gnss_ubx_configure_rover(uart_port_t uart_num)
{
    bool i2c = gnss_io_is_i2c();
    ESP_LOGI(TAG, "Configuro ricevitore u-blox come rover (riceve RTCM3, emette NMEA/GGA su %s)", i2c ? "I2C" : "UART1");

    gnss_ubx_poll_version(uart_num);
    vTaskDelay(pdMS_TO_TICKS(300)); // tempo per la risposta, loggata in modo asincrono

    const ubx_cfg_kv32_t kvs_mode[] = {
        { 0x20030001, 0 },    // CFG-TMODE-MODE = 0 (disabilitato: non e' una base fissa)
    };
    ubx_valset_group(uart_num, "TMODE", kvs_mode, sizeof(kvs_mode) / sizeof(kvs_mode[0]));

    // Durante la misura della posizione base (base_measure.c) NMEA ad alta
    // precisione: 7 decimali di minuto invece di 5 (da ~2 cm a ~0,2 mm di
    // risoluzione). Solo in quel caso, per non cambiare il formato che
    // vedono le app dei rover normali. Statica: lo stack del chiamante
    // (task main) ha gia' avuto overflow con copie di app_settings_t.
    bool measuring = false;
    settings_peek(peek_measure_active, &measuring);
    // Mandata sempre, anche a 0: la configurazione va nella RAM del
    // ricevitore, che resta acceso quando l'ESP32 si riavvia, quindi
    // dopo una misura resterebbe attiva.
    const ubx_cfg_kv32_t kvs_hp[] = {
        { 0x10930006, measuring ? 1u : 0u },    // CFG-NMEA-HIGHPREC
    };
    ubx_valset_group(uart_num, "NMEA alta precisione", kvs_hp, 1);

    // Costellazioni abilitate esplicitamente: mai state toccate prima
    // d'ora in questo file. Osservato in pratica che GLONASS e Galileo
    // non emettevano MAI una sentenza GSV, nemmeno vuota (a differenza
    // di BeiDou, che compariva comunque con "00" satelliti) - indizio
    // che fossero disabilitate a livello di sistema GNSS, non solo
    // "senza segnale". Gruppo separato: se questo modulo non supporta
    // una di queste costellazioni, un NAK qui non deve far cadere anche
    // il resto della configurazione (UART/messaggi) come succedeva
    // prima con l'invio unico.
    const ubx_cfg_kv32_t kvs_signals[] = {
        { 0x1031001f, 1 },    // CFG-SIGNAL-GPS_ENA
        { 0x10310021, 1 },    // CFG-SIGNAL-GAL_ENA (Galileo)
        { 0x10310025, 1 },    // CFG-SIGNAL-GLO_ENA (GLONASS)
        { 0x10310022, 1 },    // CFG-SIGNAL-BDS_ENA (BeiDou)
    };
    ubx_valset_group(uart_num, "SIGNAL (costellazioni)", kvs_signals, sizeof(kvs_signals) / sizeof(kvs_signals[0]));

    if (i2c) {
        // Ricevitore collegato via I2C (es. HAT Syneda uRTK6.0): stessa
        // configurazione dei gruppi UART sotto, ma sulla porta I2C. Le UART
        // del modulo non arrivano all'ESP32, inutile configurarle.
        const ubx_cfg_kv32_t kvs_i2c[] = {
            { 0x10710004, 1 },    // CFG-I2CINPROT-RTCM3X: accetta RTCM3 in ingresso via I2C
            { 0x10720004, 0 },    // CFG-I2COUTPROT-RTCM3X
            { 0x10720002, 1 },    // CFG-I2COUTPROT-NMEA
            { 0x209100ba, 1 },    // CFG-MSGOUT-NMEA_ID_GGA_I2C
            { 0x209100c4, 1 },    // CFG-MSGOUT-NMEA_ID_GSV_I2C
        };
        ubx_valset_group(uart_num, "I2C", kvs_i2c, sizeof(kvs_i2c) / sizeof(kvs_i2c[0]));
        return ESP_OK;
    }

    const ubx_cfg_kv32_t kvs_uart1[] = {
        { 0x10730004, 1 },    // CFG-UART1INPROT-RTCM3X: accetta RTCM3 in ingresso su UART1
        { 0x10740004, 0 },    // CFG-UART1OUTPROT-RTCM3X: non serve piu' emettere RTCM3
        { 0x10740002, 1 },    // CFG-UART1OUTPROT-NMEA: riabilita NMEA in uscita (per il $GxGGA)
        { 0x209100bb, 1 },    // CFG-MSGOUT-NMEA_ID_GGA_UART1: abilitare il protocollo non basta,
                               // serve anche abilitare esplicitamente il messaggio GGA (1 = ogni epoca)
        { 0x209100c5, 1 },    // CFG-MSGOUT-NMEA_ID_GSV_UART1: idem per GSV, da cui gnss_signal.c
                               // ricava l'elenco satelliti/SNR mostrato in /api/signals
    };
    ubx_valset_group(uart_num, "UART1", kvs_uart1, sizeof(kvs_uart1) / sizeof(kvs_uart1[0]));

    // Stessa configurazione ripetuta per UART2. Non sappiamo con certezza
    // su quale delle due UART fisiche del modulo sia cablata questa
    // specifica breakout board (non verificabile da remoto) - configurare
    // la porta sbagliata avrebbe fatto sembrare questi comandi applicati
    // mentre in realta' non toccavano la porta giusta. Gruppo separato per
    // lo stesso motivo del blocco SIGNAL sopra.
    const ubx_cfg_kv32_t kvs_uart2[] = {
        { 0x10750004, 1 },    // CFG-UART2INPROT-RTCM3X
        { 0x10760004, 0 },    // CFG-UART2OUTPROT-RTCM3X
        { 0x10760002, 1 },    // CFG-UART2OUTPROT-NMEA
        { 0x209100bc, 1 },    // CFG-MSGOUT-NMEA_ID_GGA_UART2
        { 0x209100c6, 1 },    // CFG-MSGOUT-NMEA_ID_GSV_UART2
    };
    ubx_valset_group(uart_num, "UART2", kvs_uart2, sizeof(kvs_uart2) / sizeof(kvs_uart2[0]));

    // CFG-HW-ANT_CFG_VOLTCTRL rimossa: dopo averla attivata i satelliti
    // visti (prima almeno 1 con SNR reale, in modo intermittente) sono
    // scesi a zero costante anche in condizioni migliori (vicino
    // finestra) - correlazione sospetta con la regressione osservata,
    // da capire prima di riattivarla.

    return ESP_OK;
}
