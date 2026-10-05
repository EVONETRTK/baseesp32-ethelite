#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "cJSON.h"
#include "sys_stats.h"
#include "esp_system.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "sdkconfig.h"
#include "version.h"
#include "settings.h"
#include "status.h"
#include "log_buffer.h"
#include "status_led.h"
#include "oled_display.h"
#include "reset_button.h"
#include "net_manager.h"
#include "web_ui.h"
#include "ota_update.h"
#include "sd_update.h"
#include "gnss_driver.h"
#include "gnss_signal.h"
#include "gnss_ubx_ack.h"
#include "gnss_i2c.h"
#include "gnss_io.h"
#include "base_stream_demux.h"
#include "gnss_fix.h"
#include "nmea_udp_broadcast.h"
#include "gnss_nmea_reader.h"
#include "ntrip_client.h"
#include "ntrip_rover_client.h"
#include "alerts.h"
#include "base_monitor.h"
#include "rtcm3_1005.h"
#include "rtcm3_stats.h"
#include "ntrip_caster_server.h"
#include "ppp_log.h"
#include "auto_update.h"
#include "diag_log.h"
#include "fw_archive.h"
#include "sd_mutex.h"
#include "base_measure.h"
#include "time_sync.h"
#include "data_usage.h"
#include "sim_plan.h"
#include "sim_tools.h"
#include "config_backup.h"
#include "raw_log.h"

static const char *TAG = "main";

#define UART_RX_BUF_SIZE 1024

static StreamBufferHandle_t rtcm_stream;
static uart_port_t s_gnss_uart_num;

// A differenza dei pin del modem/Ethernet (fissati dallo shield, restano
// solo in Kconfig), il collegamento al GNSS esterno (seriale con i suoi
// pin, oppure I2C) varia per installazione ed e' configurabile a runtime
// dalla UI web (scheda Hardware) - vedi settings.h e gnss_io.c.
static void gnss_uart_init(const app_settings_t *settings)
{
    s_gnss_uart_num = (uart_port_t) settings->gnss_uart_num;
    gnss_io_init(settings);
}

// Un frame RTCM3 valido (CRC gia' verificato da base_stream_demux.c): va al
// caster e agli altri consumatori. base_monitor_feed() e' pensata per essere
// veloce/non bloccante (non fa I/O), per non rallentare il task.
static void base_forward_rtcm_frame(const uint8_t *frame, size_t len)
{
    status_note_rtcm_bytes((uint32_t) len);
    base_monitor_feed(frame, len);
    rtcm3_stats_feed(frame, len);
    ntrip_caster_server_feed(frame, len);
    ppp_log_feed(frame, len);
    xStreamBufferSend(rtcm_stream, frame, len, pdMS_TO_TICKS(1000));
}

// Modalita' BASE: legge il flusso del GNSS, lo separa (base_stream_demux.c)
// e inoltra al caster solo i frame RTCM3 validi. Le righe NMEA GGA/GSV (che
// il ricevitore puo' mandare insieme all'RTCM) alimentano stato fix e
// grafico satelliti, le risposte UBX vengono scartate.
static void gnss_uart_task(void *arg)
{
    uint8_t buf[UART_RX_BUF_SIZE];
    while (1) {
        sys_stats_heartbeat(HB_GNSS);
        int len = gnss_io_read(buf, sizeof(buf), pdMS_TO_TICKS(100));
        // Ogni byte passa anche dal parser ACK/NAK UBX (gnss_ubx.c attende
        // la risposta ai comandi di configurazione mandati all'avvio), solo
        // in osservazione: la separazione dei flussi la fa il demux.
        for (int i = 0; i < len; i++) {
            (void) gnss_ubx_ack_feed_byte(buf[i]);
        }
        if (len > 0) {
            base_stream_demux_feed(buf, (size_t) len, base_forward_rtcm_frame);
            raw_log_feed(buf, (size_t) len); // dati grezzi per il PPP, se la registrazione e' attiva
        }
    }
}

static void fw_archive_save_current_task(void *arg)
{
    // Non urgente: si aspetta che l'avvio sia finito (configurazione del
    // ricevitore, rete, caster). Fatta subito, la copia di ~1,5 MB sulla SD
    // coincideva esattamente con gli errori I2C verso il ricevitore u-blox
    // all'avvio (visto sul dispositivo, 1.19.68).
    vTaskDelay(pdMS_TO_TICKS(30000));
    fw_archive_save_current();
    ESP_LOGI(TAG, "Archivio firmware: stack libero minimo %u byte", (unsigned) uxTaskGetStackHighWaterMark(NULL));
    vTaskDelete(NULL);
}

// Configurazione letta all'avvio da app_main: statica, non sullo stack del
// task main (2,4 KB; con le copie annidate dell'avvio lo stack di main si
// riempiva: crash della 1.19.113 al primo avvio, annullato dal bootloader).
static app_settings_t s_boot_cfg;

// cJSON alloca moltissimi pezzi piccoli (ogni campo dello stato, delle
// impostazioni...): con la soglia della PSRAM a 4 KB finivano tutti nella
// RAM interna, che con 8 browser aperti scendeva a 15 KB (collaudo del
// 03/10/2026). Ora vanno nella PSRAM, se c'e'.
static void *cjson_malloc_psram(size_t size)
{
    void *p = heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(size);
}

void app_main(void)
{
    {
        cJSON_Hooks hooks = { .malloc_fn = cjson_malloc_psram, .free_fn = free };
        cJSON_InitHooks(&hooks);
    }
    log_buffer_init(); // il prima possibile, per non perdere i log di avvio
    time_sync_set_timezone(); // ora locale gia' dalle prime righe del log
    sd_mutex_init(); // prima che qualunque cosa possa toccare la SD (vedi sd_mutex.h)
    // Ogni montaggio della SD (ogni 30 s, log diagnostico) stampava 8 righe
    // informative di questi due moduli: riempivano il log in memoria (8 KB)
    // e cancellavano i messaggi di avvio. Restano visibili avvisi ed errori.
    esp_log_level_set("gpio", ESP_LOG_WARN);
    esp_log_level_set("sdspi_transaction", ESP_LOG_WARN);

    ESP_LOGI(TAG, "EVONETRTK firmware v%s", FIRMWARE_VERSION);
    sys_stats_boot_report();
    sys_stats_crash_report();

    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    settings_init();
    config_backup_restore_if_missing(); // configurazione persa: ripristino dalla microSD
    config_backup_crash_guard();        // riavvii a catena: ultima configurazione buona
    ota_update_check_rollback();
    gnss_signal_init();
    gnss_ubx_ack_init();
    gnss_fix_init();
    // Va inizializzato qui, incondizionatamente (non solo nel ramo base
    // sotto): /api/signals chiama rtcm3_stats_get() a prescindere dalla
    // modalita' (vedi web_ui.c), quindi serve un mutex gia' creato anche
    // in modalita' rover, dove rtcm3_stats_feed() semplicemente non verra'
    // mai chiamata e la tabella restera' vuota.
    rtcm3_stats_init();

    status_led_start();
    oled_display_start();
    reset_button_start();
    {
        // Diagnostica: ricevitore u-blox via I2C (HAT sul connettore a 40
        // pin) sullo stesso bus dell'OLED, vedi gnss_i2c.h.
        // Stessa copia usata anche per la modalita' di questo avvio (per la
        // UI: etichetta BASE/ROVER e avviso "riavvio necessario"): ogni
        // app_settings_t in piu' qui pesa ~1.7 KB sullo stack del task main,
        // gia' andato in overflow per questo (vedi sdkconfig.defaults).
        settings_get_into(&s_boot_cfg);
        status_set_active_rover(s_boot_cfg.device_mode == DEVICE_MODE_ROVER);
        gnss_i2c_probe(s_boot_cfg.oled_sda_pin, s_boot_cfg.oled_scl_pin);
    }

    // Porta su l'AP di setup + tenta WiFi/cellulare/Ethernet in background
    // (non blocca): la UI web deve restare raggiungibile anche senza rete
    // configurata.
    net_manager_start();
    time_sync_start(); // ora vera via NTP (log, temperatura massima)
    nmea_udp_broadcast_init();
    web_ui_start();

    // A questo punto AP di setup e server web sono su: il firmware si e'
    // dimostrato funzionante quanto basta per essere raggiungibile e
    // riconfigurabile. Confermalo al bootloader cosi' un aggiornamento
    // riuscito non torni indietro da solo al riavvio successivo (il
    // rollback automatico scatta solo per immagini mai confermate).
    ota_update_start_confirm();

    // Controllo automatico della scheda microSD ad ogni avvio: usa la
    // stessa funzione gia' usata dal pulsante manuale nella UI web (stesso
    // timeout di sicurezza, stessa rinomina firmware.bin -> .applied dopo
    // un aggiornamento riuscito per non riapplicarlo ad ogni riavvio) -
    // se non c'e' nessuna scheda inserita, o non c'e' un aggiornamento piu'
    // recente di quello attuale, non succede nulla.
    char sd_msg[96] = {0};
    if (sd_update_check_and_apply(sd_msg, sizeof(sd_msg))) {
        ESP_LOGI(TAG, "Firmware aggiornato da microSD all'avvio, riavvio in corso: %s", sd_msg);
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    // Archivia il firmware attualmente in esecuzione sulla SD ad ogni
    // avvio (non solo prima di un aggiornamento, come faceva finora) -
    // richiesto dall'utente: cosi' una versione funzionante resta sempre
    // disponibile sulla SD per un ripristino manuale, indipendentemente da
    // come questo firmware sia arrivato sul dispositivo (flash USB diretto
    // incluso, non solo aggiornamenti applicati dal firmware stesso). Se
    // questa versione e' gia' archiviata non riscrive nulla (vedi
    // fw_archive_save_current()) - costo trascurabile ad ogni riavvio
    // successivo al primo.
    //
    // Chiamata in un task dedicato con stack generoso, MAI direttamente
    // qui: lo stack di app_main() (task "main") e' troppo piccolo per le
    // operazioni SD/FAT che fa questa funzione - confermato da uno stack
    // overflow reale su hardware la prima volta che l'ho chiamata inline
    // qui, stessa causa/fix gia' vista piu' volte in questo stesso
    // progetto per operazioni SD analoghe (vedi net_manager.c/web_ui.c).
    // Puo' partire subito, senza ritardi artificiali per evitare altri
    // moduli SD (diag_log incluso): il mutex condiviso (sd_mutex.h) fa
    // aspettare il proprio turno invece di fallire se la scheda e'
    // gia' in uso da un altro modulo in quel momento.
    xTaskCreate(fw_archive_save_current_task, "fw_archive_boot", 8192, NULL, 2, NULL);
    diag_log_start();
    raw_log_init(); // registrazione dei dati grezzi (riprende una sessione in corso)
    sys_stats_monitor_start();
    data_usage_start();
    sim_plan_start();
    sim_tools_start();
    config_backup_start();

    app_settings_t *const settings_p = &s_boot_cfg;
    settings_get_into(settings_p);
    gnss_uart_init(settings_p);

    // Il task che legge la UART va avviato PRIMA di gnss_driver_configure():
    // e' lui a inoltrare i byte al parser ACK/NAK (gnss_ubx_ack.c) usato da
    // ubx_valset() per verificare che il ricevitore accetti davvero i
    // comandi mandati - se il task parte dopo, nessuno legge la UART nella
    // finestra in cui si aspetta la risposta e il timeout scatta sempre,
    // indipendentemente da cosa risponda il modulo (bug reale, corretto
    // qui: prima l'ordine era invertito).
    if (settings_p->device_mode == DEVICE_MODE_ROVER) {
        // Rover: un solo task legge la UART e smista lo stream NMEA a
        // broadcast UDP (AgOpenGPS/AgIO), inoltro GGA al caster, e stato
        // satelliti per la UI (vedi gnss_nmea_reader.c). Il client NTRIP
        // scrive solo l'RTCM3 ricevuto verso il GNSS, non tocca piu' la
        // UART in lettura. Niente Bluetooth Classic su questa scheda
        // (ESP32-S3 non lo supporta, solo BLE non implementata qui): su
        // iOS/qualunque piattaforma resta il broadcast UDP via WiFi/Ethernet.
        // Stack alzato a 4608 (era 4096) quando e' stato aggiunto il parsing
        // delle righe #INSPVAXA/#HEADINGA Bynav (buffer riga passato da 128
        // a 320 byte per non troncarle, vedi gnss_nmea_reader.c) - margine
        // verificato con build (uso flash/RAM), non ancora su stack reale
        // via log HWM essendo il path Bynav M21D non esercitato su hardware.
        // 6144 (era 4608): oltre alla lettura, parsing NMEA con i decimali,
        // inoltro del GGA al caster e NMEA in UDP. Restavano 420 byte liberi
        // (collaudo del rover del 05/10/2026).
        xTaskCreate(gnss_nmea_reader_task, "nmea_reader", 6144,
                    (void *)(intptr_t) s_gnss_uart_num, 6, NULL);
    } else {
        rtcm3_1005_init();
        rtcm_stream = xStreamBufferCreate(4096, 1);
        xTaskCreate(gnss_uart_task, "gnss_uart", 5120, NULL, 10, NULL); // 5 KB: con 4 ne restavano ~600 (misurato)
    }

    gnss_driver_configure(s_gnss_uart_num, settings_p->gnss_chip, settings_p->device_mode);

    if (settings_p->device_mode == DEVICE_MODE_ROVER) {
        // 4096 andava in overflow su hardware reale in ntrip_rover_connect()
        // esattamente al fallimento della DNS lookup (getaddrinfo() e'
        // gia' di per se' pesante di stack su lwIP) - confermato dopo aver
        // aggiunto le chiamate a status_ntrip_note_disconnected() (v1.6.3),
        // che hanno fatto traboccare un margine gia' stretto. Stesso motivo
        // gia' incontrato piu' volte in questo progetto per altri task.
        xTaskCreate(ntrip_rover_client_task, "ntrip_rover", 8192,
                    (void *)(intptr_t) s_gnss_uart_num, 5, NULL);
        base_measure_start_if_active(); // misura della posizione base in corso
    } else {
        xTaskCreate(ntrip_client_task, "ntrip_client", 8192, rtcm_stream, 5, NULL);
        ntrip_caster_server_start(); // non fa nulla se disattivato in settings
    }

    alerts_start();
    auto_update_start(); // non fa nulla finche' non attivato dalla UI web (settings.auto_update_check_enable)
    // Margine rimasto sullo stack di main dopo l'avvio (crash della 1.19.113).
    ESP_LOGI(TAG, "Avvio completato: stack libero minimo del task main %u byte",
             (unsigned) uxTaskGetStackHighWaterMark(NULL));
}
