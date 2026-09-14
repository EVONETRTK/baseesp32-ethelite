#include "wifi_link.h"
#include "settings.h"

#include <string.h>

#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "freertos/semphr.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "wifi_link";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static volatile bool s_connected = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
// Serializza i tentativi di connessione: net_manager_task ne fa uno in
// automatico ogni 10-15s in background, la UI web puo' chiederne uno
// manuale (es. pulsante "Connetti" per testare subito senza riavviare) -
// senza questo lock potrebbero correre in parallelo sulla stessa
// interfaccia STA.
static SemaphoreHandle_t s_wifi_mutex;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *) event_data;
        ESP_LOGW(TAG, "STA disconnessa, motivo=%d", disc ? disc->reason : -1);
        s_connected = false;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        // Il ritentativo e' compito di net_manager_task (poll ogni 3s +
        // connect con timeout pieno): riconnettersi qui a raffica su ogni
        // disconnessione, senza alcuna pausa, causava un ciclo continuo
        // auth->assoc->run->init ogni ~2-3s confermato su hardware reale,
        // mai abbastanza stabile da completare l'handshake.
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "STA IP ottenuto: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client connesso all'AP di setup");
    }
}

void wifi_link_init(void)
{
    s_events = xEventGroupCreate();
    s_wifi_mutex = xSemaphoreCreateMutex();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    app_settings_t settings = settings_get();

    // L'AP di setup resta sempre attivo (modalita' APSTA): la UI web e'
    // cosi' raggiungibile in campo anche se WiFi/GPRS non sono ancora
    // configurati o non funzionano.
    wifi_config_t ap_config = { 0 };
    strncpy((char *) ap_config.ap.ssid, settings.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = (uint8_t) strlen(settings.ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    if (strlen(settings.ap_password) >= 8) {
        strncpy((char *) ap_config.ap.password, settings.ap_password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config_t sta_config = { 0 };
    strncpy((char *) sta_config.sta.ssid, settings.wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *) sta_config.sta.password, settings.wifi_password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP di setup attivo: SSID=%s IP=192.168.4.1", settings.ap_ssid);
}

// Corpo comune a wifi_link_connect() e wifi_link_connect_with(): va sempre
// chiamato con s_wifi_mutex gia' preso, per non correre mai in parallelo
// con un altro tentativo di connessione (automatico o manuale).
static bool do_connect_locked(uint32_t timeout_ms)
{
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect fallito: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

bool wifi_link_connect(uint32_t timeout_ms)
{
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    bool ok = do_connect_locked(timeout_ms);
    xSemaphoreGive(s_wifi_mutex);
    return ok;
}

bool wifi_link_connect_with(const char *ssid, const char *password, uint32_t timeout_ms)
{
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);

    // Il driver rifiuta esp_wifi_connect() se la STA e' gia' connessa a
    // un'altra rete ("sta is connected, disconnect before connecting to
    // new ap", confermato su hardware reale con "Connetti" mentre gia'
    // agganciati a una rete precedente) - va scollegata esplicitamente
    // prima di provare la nuova.
    if (s_connected) {
        esp_wifi_disconnect();
        s_connected = false;
    }

    wifi_config_t sta_config = { 0 };
    strncpy((char *) sta_config.sta.ssid, ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *) sta_config.sta.password, password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    bool ok = do_connect_locked(timeout_ms);
    xSemaphoreGive(s_wifi_mutex);
    return ok;
}

void wifi_link_disconnect(void)
{
    esp_wifi_disconnect();
    s_connected = false;
}

bool wifi_link_is_connected(void)
{
    return s_connected;
}

esp_netif_t *wifi_link_get_sta_netif(void)
{
    return s_sta_netif;
}

esp_netif_t *wifi_link_get_ap_netif(void)
{
    return s_ap_netif;
}

bool wifi_link_get_rssi(int8_t *rssi)
{
    if (!s_connected) {
        return false;
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return false;
    }
    *rssi = info.rssi;
    return true;
}

#define WIFI_SCAN_MAX_RAW 32
#define WIFI_SCAN_TIMEOUT_MS 10000

static size_t wifi_link_scan_impl(wifi_scan_result_t *out, size_t max_results)
{
    // net_manager_task tenta la connessione in background in continuo
    // finche' non c'e' un WiFi configurato che funziona (riprova ogni
    // 10-15s) - una scansione manuale puo' andare in conflitto con un
    // tentativo di connessione gia' in corso (il driver WiFi non accetta
    // scan+connect contemporanei sulla stessa interfaccia). Il conflitto
    // e' transitorio: un paio di tentativi con una breve pausa bastano.
    wifi_scan_config_t scan_cfg = { .show_hidden = false };
    esp_err_t scan_err = ESP_FAIL;
    for (int attempt = 0; attempt < 3; attempt++) {
        scan_err = esp_wifi_scan_start(&scan_cfg, true);
        if (scan_err == ESP_OK) {
            break;
        }
        ESP_LOGW(TAG, "Scansione WiFi fallita (tentativo %d/3): %s", attempt + 1, esp_err_to_name(scan_err));
        vTaskDelay(pdMS_TO_TICKS(500));
    }
    if (scan_err != ESP_OK) {
        return 0;
    }

    // wifi_ap_record_t e' molto piu' grande di quel che sembra (include
    // fra l'altro le info 802.11ax/HE) - un array di 32 sullo stack ha
    // causato uno stack overflow reale su hardware in piu' di un task
    // chiamante (confermato: prima "wifi_scan", poi "net_manager" non
    // appena ha iniziato a scansionare anche lui) nonostante stack gia'
    // raddoppiati piu' volte. Anziche' continuare a indovinare la
    // dimensione giusta per ogni nuovo chiamante, il buffer va sull'heap:
    // stessa logica, nessun limite di stack da azzeccare.
    uint16_t num = WIFI_SCAN_MAX_RAW;
    wifi_ap_record_t *raw = malloc(WIFI_SCAN_MAX_RAW * sizeof(wifi_ap_record_t));
    if (!raw) {
        ESP_LOGE(TAG, "Memoria insufficiente per la scansione WiFi");
        return 0;
    }
    if (esp_wifi_scan_get_ap_records(&num, raw) != ESP_OK) {
        free(raw);
        return 0;
    }

    // Dedup per SSID (piu' access point/mesh possono trasmettere lo
    // stesso nome su canali diversi) tenendo il segnale migliore,
    // risultato ordinato per segnale decrescente come restituito da
    // esp_wifi_scan_get_ap_records().
    size_t count = 0;
    for (uint16_t i = 0; i < num && count < max_results; i++) {
        if (raw[i].ssid[0] == '\0') {
            continue; // rete nascosta, show_hidden=false ma per sicurezza
        }
        bool dup = false;
        for (size_t j = 0; j < count; j++) {
            if (strcmp(out[j].ssid, (const char *) raw[i].ssid) == 0) {
                dup = true;
                break;
            }
        }
        if (dup) {
            continue;
        }
        strncpy(out[count].ssid, (const char *) raw[i].ssid, sizeof(out[count].ssid) - 1);
        out[count].ssid[sizeof(out[count].ssid) - 1] = '\0';
        out[count].rssi = raw[i].rssi;
        out[count].secure = (raw[i].authmode != WIFI_AUTH_OPEN);
        count++;
    }
    free(raw);
    return count;
}

bool wifi_link_connect_known(uint32_t connect_timeout_ms)
{
    app_settings_t settings = settings_get();

    wifi_scan_result_t results[WIFI_SCAN_MAX_RAW];
    size_t n = wifi_link_scan_impl(results, WIFI_SCAN_MAX_RAW);
    if (n == 0) {
        // Scansione vuota/fallita: ripiega sul tentativo diretto di
        // sempre, non e' detto che significhi "nessuna rete nota qui".
        return wifi_link_connect(connect_timeout_ms);
    }

    // Reti note da provare, in ordine di preferenza: la principale prima,
    // poi le altre gia' collegate con successo in passato.
    const char *cand_ssid[1 + WIFI_KNOWN_NETWORKS_MAX];
    const char *cand_pass[1 + WIFI_KNOWN_NETWORKS_MAX];
    size_t num_cand = 0;
    if (settings.wifi_ssid[0] != '\0') {
        cand_ssid[num_cand] = settings.wifi_ssid;
        cand_pass[num_cand] = settings.wifi_password;
        num_cand++;
    }
    for (int i = 0; i < WIFI_KNOWN_NETWORKS_MAX; i++) {
        if (settings.wifi_known_networks[i].ssid[0] != '\0') {
            cand_ssid[num_cand] = settings.wifi_known_networks[i].ssid;
            cand_pass[num_cand] = settings.wifi_known_networks[i].password;
            num_cand++;
        }
    }

    // Tra le reti note, sceglie quella col segnale migliore fra quelle
    // effettivamente viste in questa scansione (non semplicemente la
    // prima della lista) - piu' probabile che la connessione riesca al
    // primo colpo.
    int best_result = -1;
    size_t best_cand = 0;
    for (size_t c = 0; c < num_cand; c++) {
        for (size_t r = 0; r < n; r++) {
            if (strcmp(cand_ssid[c], results[r].ssid) == 0) {
                if (best_result < 0 || results[r].rssi > results[best_result].rssi) {
                    best_result = (int) r;
                    best_cand = c;
                }
                break;
            }
        }
    }

    if (best_result < 0) {
        ESP_LOGI(TAG, "Nessuna rete nota visibile in questa scansione (%u reti viste)", (unsigned) n);
        return false;
    }

    ESP_LOGI(TAG, "Rete nota trovata: '%s' (%d dBm)", cand_ssid[best_cand], results[best_result].rssi);
    return wifi_link_connect_with(cand_ssid[best_cand], cand_pass[best_cand], connect_timeout_ms);
}

// Contesto allocato sull'heap (non sullo stack del chiamante): se scatta
// il timeout, il task puo' finire piu' tardi e scrive comunque in memoria
// valida - mai nel buffer "out" del chiamante, che a quel punto potrebbe
// gia' essere uscito di scope (stack della richiesta HTTP).
typedef struct {
    wifi_scan_result_t out[WIFI_SCAN_MAX_RAW];
    size_t count;
    SemaphoreHandle_t done;
} wifi_scan_task_ctx_t;

static void wifi_scan_task(void *arg)
{
    wifi_scan_task_ctx_t *ctx = (wifi_scan_task_ctx_t *) arg;
    ctx->count = wifi_link_scan_impl(ctx->out, WIFI_SCAN_MAX_RAW);
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

// Una scansione manuale puo' andare in stallo per un tempo imprevedibile
// (anche oltre 30s, confermato su hardware reale) quando va in conflitto
// col tentativo di connessione che net_manager_task fa in continuo in
// background - il driver WiFi non gestisce bene scan+connect
// contemporanei sulla stessa interfaccia. La UI web non deve mai restare
// bloccata per questo: il lavoro vero gira in un task a parte con un
// timeout massimo, stesso schema di sd_update_check_and_apply().
size_t wifi_link_scan(wifi_scan_result_t *out, size_t max_results)
{
    wifi_scan_task_ctx_t *ctx = calloc(1, sizeof(wifi_scan_task_ctx_t));
    if (!ctx) {
        return 0;
    }
    ctx->done = xSemaphoreCreateBinary();

    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, ctx, 5, NULL) != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        return 0;
    }

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout (%d ms) durante la scansione WiFi - il tentativo continua in background",
                 WIFI_SCAN_TIMEOUT_MS);
        return 0; // ctx e ctx->done restano vivi per il task orfano, vedi commento sopra
    }

    size_t n = ctx->count < max_results ? ctx->count : max_results;
    memcpy(out, ctx->out, n * sizeof(wifi_scan_result_t));
    vSemaphoreDelete(ctx->done);
    free(ctx);
    return n;
}
