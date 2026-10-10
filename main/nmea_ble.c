#include "nmea_ble.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/message_buffer.h"
#include "freertos/idf_additions.h"

#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_bt.h"
#include "nimble/nimble_port.h"
#include "host/ble_hs.h"
#include "host/util/util.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"

static const char *TAG = "nmea_ble";

// Riga piu' lunga accettata (come il buffer del lettore NMEA) + CR/LF.
#define BLE_LINE_MAX 320
// Coda tra il lettore del ricevitore e il task che invia al telefono: ~2 s
// di frasi a 10 Hz (GGA+RMC+VTG+GSV...). In PSRAM.
#define BLE_QUEUE_BYTES 4096

// Nordic UART Service (NUS): UUID a 128 bit, byte al contrario
// (little-endian) come vuole BLE_UUID128_INIT.
// 6E400001-B5A3-F393-E0A9-E50E24DCCA9E servizio
// 6E400002-...                          RX (il telefono scrive: ignorato)
// 6E400003-...                          TX (notifiche verso il telefono)
static const ble_uuid128_t s_nus_svc_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x01, 0x00, 0x40, 0x6E);
static const ble_uuid128_t s_nus_rx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x02, 0x00, 0x40, 0x6E);
static const ble_uuid128_t s_nus_tx_uuid =
    BLE_UUID128_INIT(0x9E, 0xCA, 0xDC, 0x24, 0x0E, 0xE5, 0xA9, 0xE0, 0x93, 0xF3, 0xA3, 0xB5, 0x03, 0x00, 0x40, 0x6E);

static volatile nmea_ble_state_t s_state = NMEA_BLE_OFF;
static bool s_boot_done;     // nmea_ble_boot() gia' chiamata
static bool s_mem_released;  // memoria del Bluetooth restituita: mai piu' NimBLE
static volatile bool s_connected;
static volatile bool s_subscribed; // il telefono ha acceso le notifiche su TX
static volatile uint16_t s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
static uint16_t s_tx_val_handle;
static uint8_t s_own_addr_type;
static MessageBufferHandle_t s_queue;
static char *s_tx_buf; // in PSRAM: niente copie grandi sugli stack
static uint32_t s_dropped; // frasi scartate (coda o buffer NimBLE pieni)
static int s_mem_release_result = -1;  // esito di esp_bt_mem_release (-1 = non chiamata)
static int s_mem_released_bytes;       // RAM interna restituita all'heap

static void advertise(void);

static int nus_access(uint16_t conn_handle, uint16_t attr_handle, struct ble_gatt_access_ctxt *ctxt, void *arg)
{
    (void) conn_handle;
    (void) attr_handle;
    (void) arg;
    // RX: scritture del telefono accettate e scartate (nessun comando
    // previsto). TX: solo notifiche, una lettura restituisce vuoto.
    (void) ctxt;
    return 0;
}

static const struct ble_gatt_svc_def s_gatt_svcs[] = {
    {
        .type = BLE_GATT_SVC_TYPE_PRIMARY,
        .uuid = &s_nus_svc_uuid.u,
        .characteristics = (struct ble_gatt_chr_def[]) {
            {
                .uuid = &s_nus_rx_uuid.u,
                .access_cb = nus_access,
                .flags = BLE_GATT_CHR_F_WRITE | BLE_GATT_CHR_F_WRITE_NO_RSP,
            },
            {
                .uuid = &s_nus_tx_uuid.u,
                .access_cb = nus_access,
                .val_handle = &s_tx_val_handle,
                .flags = BLE_GATT_CHR_F_NOTIFY,
            },
            { 0 },
        },
    },
    { 0 },
};

static int gap_event(struct ble_gap_event *event, void *arg)
{
    (void) arg;
    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        if (event->connect.status == 0) {
            s_conn_handle = event->connect.conn_handle;
            s_subscribed = false;
            s_connected = true;
            ESP_LOGI(TAG, "Telefono collegato via Bluetooth");
        } else {
            advertise(); // connessione non riuscita: si riprende l'annuncio
        }
        break;
    case BLE_GAP_EVENT_DISCONNECT:
        ESP_LOGI(TAG, "Telefono scollegato (motivo %d), frasi scartate finora %u",
                 event->disconnect.reason, (unsigned) s_dropped);
        s_subscribed = false;
        s_connected = false;
        s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
        advertise();
        break;
    case BLE_GAP_EVENT_ADV_COMPLETE:
        if (!s_connected) {
            advertise();
        }
        break;
    case BLE_GAP_EVENT_SUBSCRIBE:
        if (event->subscribe.attr_handle == s_tx_val_handle) {
            s_subscribed = event->subscribe.cur_notify != 0;
            ESP_LOGI(TAG, "Notifiche NMEA %s", s_subscribed ? "accese dal telefono" : "spente dal telefono");
        }
        break;
    case BLE_GAP_EVENT_MTU:
        ESP_LOGI(TAG, "MTU negoziato %u byte", (unsigned) event->mtu.value);
        break;
    default:
        break;
    }
    return 0;
}

// Nome nell'annuncio (lo vedono tutte le app, anche con la ricerca
// passiva), UUID del servizio nella risposta alla scansione.
static void advertise(void)
{
    struct ble_hs_adv_fields fields;
    memset(&fields, 0, sizeof(fields));
    fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;
    // Annuncio di 31 byte: 3 per i flag, 2 di intestazione del nome. Un nome
    // della rete di setup piu' lungo di 26 caratteri va accorciato qui (il
    // nome intero resta nel servizio GAP, letto dopo la connessione).
    const char *name = ble_svc_gap_device_name();
    size_t name_len = strlen(name);
    fields.name = (const uint8_t *) name;
    fields.name_len = (uint8_t) (name_len > 26 ? 26 : name_len);
    fields.name_is_complete = name_len <= 26;
    int rc = ble_gap_adv_set_fields(&fields);
    if (rc != 0) {
        ESP_LOGW(TAG, "Dati dell'annuncio non impostati (%d)", rc);
        return;
    }

    struct ble_hs_adv_fields rsp;
    memset(&rsp, 0, sizeof(rsp));
    rsp.uuids128 = (ble_uuid128_t *) &s_nus_svc_uuid;
    rsp.num_uuids128 = 1;
    rsp.uuids128_is_complete = 1;
    rc = ble_gap_adv_rsp_set_fields(&rsp);
    if (rc != 0) {
        ESP_LOGW(TAG, "Risposta alla scansione non impostata (%d)", rc);
    }

    struct ble_gap_adv_params adv;
    memset(&adv, 0, sizeof(adv));
    adv.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv.disc_mode = BLE_GAP_DISC_MODE_GEN;
    rc = ble_gap_adv_start(s_own_addr_type, NULL, BLE_HS_FOREVER, &adv, gap_event, NULL);
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "Annuncio Bluetooth non avviato (%d)", rc);
    }
}

static void on_sync(void)
{
    int rc = ble_hs_util_ensure_addr(0);
    if (rc == 0) {
        rc = ble_hs_id_infer_auto(0, &s_own_addr_type);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Indirizzo Bluetooth non disponibile (%d)", rc);
        s_state = NMEA_BLE_ERROR;
        return;
    }
    advertise();
}

static void on_reset(int reason)
{
    ESP_LOGW(TAG, "Bluetooth reimpostato (motivo %d)", reason);
    s_subscribed = false;
    s_connected = false;
    s_conn_handle = BLE_HS_CONN_HANDLE_NONE;
}

static void host_task(void *arg)
{
    (void) arg;
    nimble_port_run(); // non ritorna
    vTaskDelete(NULL);
}

// Prende le frasi dalla coda e le manda al telefono a pezzi della
// dimensione dell'MTU negoziato (MTU - 3 byte di intestazione ATT). Se
// NimBLE non ha piu' buffer liberi si scarta il resto della frase: il
// telefono la ignora per il checksum e prende la successiva.
static void tx_task(void *arg)
{
    (void) arg;
    while (1) {
        size_t n = xMessageBufferReceive(s_queue, s_tx_buf, BLE_LINE_MAX, portMAX_DELAY);
        if (n == 0) {
            continue;
        }
        uint16_t conn = s_conn_handle;
        if (!s_subscribed || conn == BLE_HS_CONN_HANDLE_NONE) {
            continue;
        }
        s_tx_buf[n++] = '\r';
        s_tx_buf[n++] = '\n';
        uint16_t mtu = ble_att_mtu(conn);
        if (mtu <= 3) {
            continue;
        }
        size_t chunk = mtu - 3;
        for (size_t off = 0; off < n; off += chunk) {
            size_t len = n - off < chunk ? n - off : chunk;
            struct os_mbuf *om = ble_hs_mbuf_from_flat(s_tx_buf + off, (uint16_t) len);
            if (!om) {
                s_dropped++;
                break;
            }
            if (ble_gatts_notify_custom(conn, s_tx_val_handle, om) != 0) { // om liberato da NimBLE
                s_dropped++;
                break;
            }
        }
    }
}

void nmea_ble_boot(bool will_start)
{
    if (s_boot_done) {
        return; // la memoria si restituisce una volta sola (regioni doppie nell'heap)
    }
    s_boot_done = true;
    if (will_start) {
        return;
    }
    // Controller mai inizializzato: si puo' restituire la sua RAM (e quella
    // di NimBLE) all'heap. Dopo, nessuna funzione Bluetooth va piu' usata.
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);
    esp_err_t err = esp_bt_mem_release(ESP_BT_MODE_BLE);
    s_mem_released = true;
    s_mem_release_result = (int) err;
    s_mem_released_bytes = (int) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - free_before);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Uscita Bluetooth spenta: restituiti %d byte di RAM interna",
                 (int) (heap_caps_get_free_size(MALLOC_CAP_INTERNAL) - free_before));
    } else {
        ESP_LOGW(TAG, "Memoria del Bluetooth non restituita: %s", esp_err_to_name(err));
    }
}

void nmea_ble_start(const char *name)
{
    if (s_mem_released || s_state != NMEA_BLE_OFF) {
        return;
    }
    s_boot_done = true; // da qui la memoria del Bluetooth non si restituisce piu'
    size_t free_before = heap_caps_get_free_size(MALLOC_CAP_INTERNAL);

    // +2 per CR/LF aggiunti dal task di invio.
    s_tx_buf = heap_caps_malloc(BLE_LINE_MAX + 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    s_queue = xMessageBufferCreateWithCaps(BLE_QUEUE_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!s_tx_buf || !s_queue) {
        ESP_LOGE(TAG, "Uscita Bluetooth non avviata: memoria insufficiente");
        s_state = NMEA_BLE_ERROR;
        return;
    }

    esp_err_t err = nimble_port_init(); // controller + host; in errore libera da solo
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Uscita Bluetooth non avviata: %s", esp_err_to_name(err));
        s_state = NMEA_BLE_ERROR;
        return;
    }

    ble_hs_cfg.sync_cb = on_sync;
    ble_hs_cfg.reset_cb = on_reset;
    ble_svc_gap_init();
    ble_svc_gatt_init();
    int rc = ble_gatts_count_cfg(s_gatt_svcs);
    if (rc == 0) {
        rc = ble_gatts_add_svcs(s_gatt_svcs);
    }
    if (rc == 0) {
        rc = ble_svc_gap_device_name_set(name && name[0] ? name : "EVONETRTK");
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "Servizio Bluetooth non registrato (%d)", rc);
        nimble_port_deinit();
        s_state = NMEA_BLE_ERROR;
        return;
    }

    // Task di NimBLE e di invio con lo stack in PSRAM (non scrivono mai in
    // flash: niente accoppiamento ne' chiavi salvate). Non si usa
    // nimble_port_freertos_init(): senza memoria per il task abortirebbe.
    TaskHandle_t host = NULL;
    if (xTaskCreatePinnedToCoreWithCaps(host_task, "nimble_host", 4096, NULL, configMAX_PRIORITIES - 4, &host,
                                        CONFIG_BT_NIMBLE_PINNED_TO_CORE, MALLOC_CAP_SPIRAM) != pdPASS) {
        ESP_LOGE(TAG, "Uscita Bluetooth non avviata: memoria insufficiente per il task");
        nimble_port_deinit();
        s_state = NMEA_BLE_ERROR;
        return;
    }
    if (xTaskCreateWithCaps(tx_task, "nmea_ble_tx", 3072, NULL, 4, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        // Annuncio attivo ma niente invio: meglio dichiararlo in errore.
        ESP_LOGE(TAG, "Task di invio Bluetooth non avviato: memoria insufficiente");
        s_state = NMEA_BLE_ERROR;
        return;
    }
    s_state = NMEA_BLE_ON;
    ESP_LOGI(TAG, "Uscita NMEA via Bluetooth LE attiva come \"%s\" (RAM interna usata ~%d byte)",
             ble_svc_gap_device_name(), (int) (free_before - heap_caps_get_free_size(MALLOC_CAP_INTERNAL)));
}

void nmea_ble_send(const char *line, size_t len)
{
    // La riga del lettore puo' finire con '\r': CR/LF li aggiunge il task di invio.
    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) {
        len--;
    }
    if (!s_subscribed || s_state != NMEA_BLE_ON || len == 0 || len > BLE_LINE_MAX) {
        return;
    }
    // Attesa 0: con la coda piena la frase si perde, il lettore non si ferma.
    if (xMessageBufferSend(s_queue, line, len, 0) != len) {
        s_dropped++;
    }
}

nmea_ble_state_t nmea_ble_state(void)
{
    return s_state;
}

const char *nmea_ble_state_str(nmea_ble_state_t st)
{
    switch (st) {
    case NMEA_BLE_ON: return "attivo";
    case NMEA_BLE_ERROR: return "errore";
    default: return "spento";
    }
}

bool nmea_ble_client_connected(void)
{
    return s_state == NMEA_BLE_ON && s_connected;
}

void nmea_ble_mem_info(int *result, int *released_bytes)
{
    *result = s_mem_release_result;
    *released_bytes = s_mem_released_bytes;
}
