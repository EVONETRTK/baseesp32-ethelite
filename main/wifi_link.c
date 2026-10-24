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
#include "ping/ping_sock.h"

static const char *TAG = "wifi_link";

#define WIFI_CONNECTED_BIT BIT0

// Password della rete di setup (AP) usata se quella salvata non e' valida
// per WPA2 (meno di 8 o piu' di 63 caratteri): e' quella di fabbrica,
// stampata sulla scheda di accesso. Prima con una password corta l'AP
// diventava APERTO senza avvisi, e con una troppo lunga veniva troncata.
#define AP_FALLBACK_PASSWORD "baseesp32setup"

static EventGroupHandle_t s_events;
static volatile bool s_connected = false;
static bool s_inited;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;
// Serializza i tentativi di connessione: net_manager_task ne fa uno in
// automatico ogni 10-15s in background, la UI web puo' chiederne uno
// manuale (es. pulsante "Connetti" per testare subito senza riavviare) -
// senza questo lock potrebbero correre in parallelo sulla stessa
// interfaccia STA.
static SemaphoreHandle_t s_wifi_mutex;

// Dispositivi collegati alla rete di setup (AP): senza nessuno, le frasi NMEA
// in UDP non si trasmettono anche li' (nmea_udp_broadcast.c).
static volatile int s_ap_clients;
static volatile uint32_t s_sta_disconnects; // cadute della STA da questo avvio

uint32_t wifi_link_sta_disconnects(void)
{
    return s_sta_disconnects;
}

int wifi_link_ap_clients(void)
{
    return s_ap_clients;
}

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        wifi_event_sta_disconnected_t *disc = (wifi_event_sta_disconnected_t *) event_data;
        ESP_LOGW(TAG, "STA disconnessa, motivo=%d", disc ? disc->reason : -1);
        if (s_connected) {
            s_sta_disconnects++; // solo cadute da collegato, non tentativi falliti
        }
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
        s_ap_clients++;
        ESP_LOGI(TAG, "Client connesso all'AP di setup");
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STADISCONNECTED) {
        if (s_ap_clients > 0) {
            s_ap_clients--;
        }
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STOP) {
        s_ap_clients = 0;
    }
}

// Rete aperta (password vuota): soglia di sicurezza "aperta", altrimenti
// con la soglia WPA2 il driver scartava le reti senza password e non ci si
// poteva collegare a un hotspot aperto.
static void set_sta_threshold(wifi_config_t *cfg, const char *password)
{
    cfg->sta.threshold.authmode = (password && password[0]) ? WIFI_AUTH_WPA2_PSK : WIFI_AUTH_OPEN;
}

// Solo i campi della rete, letti senza copiare tutta la configurazione
// (~2,4 KB) sullo stack del chiamante.
typedef struct {
    char ap_ssid[33];
    char ap_password[65];
    char wifi_ssid[33];
    char wifi_password[65];
} wifi_init_cfg_t;

static void peek_init_cfg(const app_settings_t *s, void *ctx)
{
    wifi_init_cfg_t *c = (wifi_init_cfg_t *) ctx;
    strlcpy(c->ap_ssid, s->ap_ssid, sizeof(c->ap_ssid));
    strlcpy(c->ap_password, s->ap_password, sizeof(c->ap_password));
    strlcpy(c->wifi_ssid, s->wifi_ssid, sizeof(c->wifi_ssid));
    strlcpy(c->wifi_password, s->wifi_password, sizeof(c->wifi_password));
}

void wifi_link_init(void)
{
    // Puo' essere chiamata anche piu' tardi da net_manager (modalita' "Solo
    // Ethernet" senza cavo: si accende comunque l'AP di setup): una volta sola.
    if (s_inited) {
        return;
    }
    s_inited = true;

    s_events = xEventGroupCreate();
    s_wifi_mutex = xSemaphoreCreateMutex();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    static wifi_init_cfg_t settings; // statica: niente struct sullo stack del chiamante
    settings_peek(peek_init_cfg, &settings);

    // L'AP di setup resta sempre attivo (modalita' APSTA): la UI web e'
    // cosi' raggiungibile in campo anche se WiFi/GPRS non sono ancora
    // configurati o non funzionano.
    wifi_config_t ap_config = { 0 };
    strncpy((char *) ap_config.ap.ssid, settings.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = (uint8_t) strlen((const char *) ap_config.ap.ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    size_t ap_pass_len = strlen(settings.ap_password);
    const char *ap_pass = settings.ap_password;
    if (ap_pass_len < 8 || ap_pass_len > 63) {
        // Mai una rete di setup aperta (da li' si raggiunge il pannello).
        ESP_LOGE(TAG, "Password della rete di setup non valida (%u caratteri, servono da 8 a 63): "
                      "uso quella di fabbrica scritta sulla scheda di accesso. Correggila nel pannello.",
                 (unsigned) ap_pass_len);
        ap_pass = AP_FALLBACK_PASSWORD;
    }
    strncpy((char *) ap_config.ap.password, ap_pass, sizeof(ap_config.ap.password) - 1);
    ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;

    wifi_config_t sta_config = { 0 };
    strncpy((char *) sta_config.sta.ssid, settings.wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *) sta_config.sta.password, settings.wifi_password, sizeof(sta_config.sta.password) - 1);
    set_sta_threshold(&sta_config, settings.wifi_password);
    memset(settings.wifi_password, 0, sizeof(settings.wifi_password));
    memset(settings.ap_password, 0, sizeof(settings.ap_password));

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());
    // Niente risparmio energetico: la base e' sempre alimentata. In modalita'
    // risparmio il WiFi "dormiva" tra un pacchetto e l'altro e il pannello a
    // tratti rispondeva in secondi (ping fino a 130 ms invece di ~15, visto il
    // 03/10/2026). Consumo un po' piu' alto, collegamento piu' reattivo.
    esp_wifi_set_ps(WIFI_PS_NONE);

    ESP_LOGI(TAG, "AP di setup attivo: SSID=%s IP=192.168.4.1", settings.ap_ssid);
}

bool wifi_link_is_started(void)
{
    return s_inited;
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
    if (!s_inited) {
        return false;
    }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    bool ok = do_connect_locked(timeout_ms);
    xSemaphoreGive(s_wifi_mutex);
    return ok;
}

bool wifi_link_connect_with(const char *ssid, const char *password, uint32_t timeout_ms)
{
    if (!s_inited) {
        return false;
    }
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
    set_sta_threshold(&sta_config, password);
    esp_wifi_set_config(WIFI_IF_STA, &sta_config);

    bool ok = do_connect_locked(timeout_ms);
    xSemaphoreGive(s_wifi_mutex);
    return ok;
}

void wifi_link_disconnect(void)
{
    if (!s_inited) {
        return;
    }
    // Sotto lo stesso lock dei tentativi di connessione: prima net_manager
    // poteva scollegare la STA nel mezzo di un "Connetti" dal pannello.
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    s_connected = false; // prima: la caduta voluta non conta come guasto
    esp_wifi_disconnect();
    xSemaphoreGive(s_wifi_mutex);
}

bool wifi_link_is_connected(void)
{
    return s_connected;
}

// --- WiFi collegato ma inutilizzabile ---------------------------------------
// Prova sul campo del 24/10/2026: dopo un avvio con l'alimentazione a scatti
// il WiFi restava "collegato" ma perdeva fino al 75% dei pacchetti (driver
// senza buffer, "m f null"; il router lo scollegava ogni 10 minuti e lui si
// ricollegava subito) per 2 ore e mezza, finche' un riavvio non l'ha
// sistemato. Con il 4G in uso nessuno se ne accorgeva: pannello irraggiungibile
// dalla rete locale. Controllo: quanti ping al router (gateway) tornano sul WiFi.

#define GW_PINGS 8

// Contesto statico, mai liberato: con esp_ping la sessione finisce in modo
// asincrono e i callback arrivano anche dopo l'attesa qui sotto. Un contesto
// sullo stack (prima versione, mai pubblicata) veniva scritto dopo il ritorno
// della funzione. Una sola sessione alla volta (s_gw.busy).
static struct {
    SemaphoreHandle_t done;
    volatile int ok;
    volatile bool busy;
    bool ever_ok; // il router ha risposto almeno una volta da questo avvio
} s_gw;

static void gw_ping_success(esp_ping_handle_t h, void *arg)
{
    (void) h;
    (void) arg;
    s_gw.ok++;
}

static void gw_ping_end(esp_ping_handle_t h, void *arg)
{
    (void) arg;
    esp_ping_delete_session(h);
    xSemaphoreGive(s_gw.done); // prima del busy: chi parte dopo trova il segnale da scartare
    s_gw.busy = false;
}

int wifi_link_gateway_reply_pct(void)
{
    if (!s_inited || !s_connected || !s_sta_netif) {
        return -1; // niente da misurare
    }
    if (s_gw.busy) {
        ESP_LOGW(TAG, "Prova del router saltata: la precedente non e' ancora finita");
        return -1;
    }
    esp_netif_ip_info_t ip;
    if (esp_netif_get_ip_info(s_sta_netif, &ip) != ESP_OK || ip.gw.addr == 0) {
        return -1;
    }
    if (!s_gw.done) {
        s_gw.done = xSemaphoreCreateBinary();
        if (!s_gw.done) {
            return -1;
        }
    }
    xSemaphoreTake(s_gw.done, 0); // un segnale rimasto da una sessione scaduta
    esp_ping_config_t cfg = ESP_PING_DEFAULT_CONFIG();
    cfg.target_addr.type = IPADDR_TYPE_V4;
    cfg.target_addr.u_addr.ip4.addr = ip.gw.addr;
    cfg.count = GW_PINGS;
    cfg.interval_ms = 200;
    cfg.timeout_ms = 800;
    cfg.task_stack_size = 2560;
    cfg.interface = esp_netif_get_netif_impl_index(s_sta_netif); // proprio dal WiFi, non dal 4G
    esp_ping_callbacks_t cbs = { .cb_args = NULL, .on_ping_success = gw_ping_success, .on_ping_end = gw_ping_end };
    esp_ping_handle_t h = NULL;
    s_gw.ok = 0;
    if (esp_ping_new_session(&cfg, &cbs, &h) != ESP_OK) {
        return -1;
    }
    s_gw.busy = true;
    esp_ping_start(h);
    // ~8 ping in ~2-8 s. Se non finisce in 10 s non si aspetta oltre: la
    // sessione si chiude da sola (gw_ping_end) e fino ad allora nessuna nuova.
    if (xSemaphoreTake(s_gw.done, pdMS_TO_TICKS(10000)) != pdTRUE) {
        return -1;
    }
    int pct = s_gw.ok * 100 / GW_PINGS;
    if (pct > 0) {
        s_gw.ever_ok = true;
    }
    return pct;
}

bool wifi_link_gateway_ever_ok(void)
{
    return s_gw.ever_ok;
}

void wifi_link_restart_radio(void)
{
    if (!s_inited) {
        return;
    }
    xSemaphoreTake(s_wifi_mutex, portMAX_DELAY);
    ESP_LOGW(TAG, "Radio WiFi spenta e riaccesa (collegata ma inutilizzabile)");
    s_connected = false; // prima: la caduta voluta non conta come guasto
    esp_wifi_disconnect();
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    // Nessun ricalcolo della calibrazione: a meta' accensione l'ESP-IDF non
    // ricalibra la radio (solo una volta per avvio). Serve il riavvio del
    // driver: buffer, associazione, stato della radio.
    esp_wifi_stop();
    esp_wifi_start();
    esp_wifi_set_ps(WIFI_PS_NONE);
    xSemaphoreGive(s_wifi_mutex);
    // La STA la ricollega net_manager, come dopo ogni caduta.
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

// Nome della rete a cui la STA e' effettivamente associata in questo
// momento (dal driver WiFi, non dalle impostazioni salvate) - puo'
// differire da settings.wifi_ssid se ci si e' collegati a una rete
// "conosciuta" diversa dalla principale (vedi wifi_link_connect_known()
// sotto). Ritorna false se non connessa.
bool wifi_link_get_current_ssid(char *out, size_t out_size)
{
    if (!s_connected) {
        return false;
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return false;
    }
    strncpy(out, (const char *) info.ssid, out_size - 1);
    out[out_size - 1] = '\0';
    return true;
}

#define WIFI_SCAN_MAX_RAW 32
#define WIFI_SCAN_TIMEOUT_MS 10000

static size_t wifi_link_scan_impl(wifi_scan_result_t *out, size_t max_results)
{
    if (!s_inited) {
        return 0; // "Solo Ethernet": radio WiFi mai avviata
    }
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

// Reti note da provare (principale + conosciute), copiate senza portare
// tutta la configurazione (~2,4 KB) sullo stack di net_manager. Statiche:
// wifi_link_connect_known() la chiama solo net_manager_task.
typedef struct {
    char ssid[1 + WIFI_KNOWN_NETWORKS_MAX][33];
    char pass[1 + WIFI_KNOWN_NETWORKS_MAX][65];
    size_t n;
    bool have_principal;
} wifi_candidates_t;

static void peek_candidates(const app_settings_t *s, void *ctx)
{
    wifi_candidates_t *c = (wifi_candidates_t *) ctx;
    c->n = 0;
    c->have_principal = false;
    if (s->wifi_ssid[0] != '\0') {
        strlcpy(c->ssid[c->n], s->wifi_ssid, sizeof(c->ssid[0]));
        strlcpy(c->pass[c->n], s->wifi_password, sizeof(c->pass[0]));
        c->n++;
        c->have_principal = true;
    }
    for (int i = 0; i < WIFI_KNOWN_NETWORKS_MAX; i++) {
        if (s->wifi_known_networks[i].ssid[0] != '\0') {
            strlcpy(c->ssid[c->n], s->wifi_known_networks[i].ssid, sizeof(c->ssid[0]));
            strlcpy(c->pass[c->n], s->wifi_known_networks[i].password, sizeof(c->pass[0]));
            c->n++;
        }
    }
}

bool wifi_link_connect_known(uint32_t connect_timeout_ms)
{
    if (!s_inited) {
        return false;
    }
    static wifi_candidates_t cand;
    static wifi_scan_result_t results[WIFI_SCAN_MAX_RAW];
    settings_peek(peek_candidates, &cand);

    size_t n = wifi_link_scan_impl(results, WIFI_SCAN_MAX_RAW);
    if (n == 0) {
        // Scansione vuota/fallita: ripiega sul tentativo diretto di
        // sempre, non e' detto che significhi "nessuna rete nota qui".
        return wifi_link_connect(connect_timeout_ms);
    }

    // La "principale" (settings.wifi_ssid, sempre candidato indice 0 se
    // presente - vedi sopra) e' quella scelta per ultima dall'utente in
    // modo esplicito (pulsante "Connetti" nella UI, tramite
    // app_settings_remember_wifi()): se e' visibile in questa scansione va
    // usata SEMPRE, indipendentemente dal segnale delle altre reti note.
    // Prima di questo fix si sceglieva semplicemente la rete nota col
    // segnale migliore fra TUTTE (principale comprese le altre "conosciute")
    // - risultato concreto osservato dall'utente: se la rete vecchia aveva
    // segnale piu' forte di quella appena scelta, il riconnettore
    // automatico (che gira di nuovo non appena finisce un test manuale)
    // ripiombava sulla rete vecchia subito dopo un cambio di rete riuscito,
    // ignorando la scelta esplicita appena fatta. Le altre reti "conosciute"
    // restano un fallback per segnale migliore SOLO se la principale non e'
    // visibile in questa scansione.
    bool have_principal = cand.have_principal;
    for (size_t r = 0; have_principal && r < n; r++) {
        if (strcmp(cand.ssid[0], results[r].ssid) == 0) {
            ESP_LOGI(TAG, "Rete principale visibile: '%s' (%d dBm)", cand.ssid[0], results[r].rssi);
            return wifi_link_connect_with(cand.ssid[0], cand.pass[0], connect_timeout_ms);
        }
    }

    int best_result = -1;
    size_t best_cand = 0;
    size_t fallback_start = have_principal ? 1 : 0;
    for (size_t c = fallback_start; c < cand.n; c++) {
        for (size_t r = 0; r < n; r++) {
            if (strcmp(cand.ssid[c], results[r].ssid) == 0) {
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

    ESP_LOGI(TAG, "Rete nota trovata (principale non visibile): '%s' (%d dBm)", cand.ssid[best_cand], results[best_result].rssi);
    return wifi_link_connect_with(cand.ssid[best_cand], cand.pass[best_cand], connect_timeout_ms);
}

// Contesto allocato sull'heap (non sullo stack del chiamante): se scatta
// il timeout, il task puo' finire piu' tardi e scrive comunque in memoria
// valida - mai nel buffer "out" del chiamante, che a quel punto potrebbe
// gia' essere uscito di scope (stack della richiesta HTTP). Chi arriva per
// ultimo (task finito o chiamante in timeout) lo libera: prima, dopo un
// timeout, contesto (~1,3 KB di RAM interna) e semaforo restavano persi.
typedef struct {
    wifi_scan_result_t out[WIFI_SCAN_MAX_RAW];
    size_t count;
    SemaphoreHandle_t done;
    bool given;     // il task ha dato il semaforo e non tocca piu' ctx (protetto da s_scan_lock)
    bool released;  // il chiamante non usa piu' ctx: risultato letto o timeout (idem)
} wifi_scan_task_ctx_t;

static portMUX_TYPE s_scan_lock = portMUX_INITIALIZER_UNLOCKED;

static void scan_ctx_free(wifi_scan_task_ctx_t *ctx)
{
    vSemaphoreDelete(ctx->done);
    free(ctx);
}

static void wifi_scan_task(void *arg)
{
    wifi_scan_task_ctx_t *ctx = (wifi_scan_task_ctx_t *) arg;
    ctx->count = wifi_link_scan_impl(ctx->out, WIFI_SCAN_MAX_RAW);
    portENTER_CRITICAL(&s_scan_lock);
    bool released = ctx->released;
    portEXIT_CRITICAL(&s_scan_lock);
    if (released) {
        scan_ctx_free(ctx); // il chiamante e' gia' andato via (timeout)
        vTaskDelete(NULL);
        return;
    }
    xSemaphoreGive(ctx->done);
    portENTER_CRITICAL(&s_scan_lock);
    ctx->given = true;
    released = ctx->released;
    portEXIT_CRITICAL(&s_scan_lock);
    if (released) {
        scan_ctx_free(ctx); // il chiamante ha finito prima di noi: liberiamo noi
    }
    vTaskDelete(NULL);
}

// Il chiamante ha finito con ctx: lo libera se il task non lo usa piu',
// altrimenti ci pensera' il task.
static void scan_ctx_release(wifi_scan_task_ctx_t *ctx)
{
    portENTER_CRITICAL(&s_scan_lock);
    ctx->released = true;
    bool task_done = ctx->given;
    portEXIT_CRITICAL(&s_scan_lock);
    if (task_done) {
        scan_ctx_free(ctx);
    }
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
    if (!ctx->done) {
        free(ctx);
        return 0;
    }

    if (xTaskCreate(wifi_scan_task, "wifi_scan", 4096, ctx, 5, NULL) != pdPASS) {
        scan_ctx_free(ctx);
        return 0;
    }

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(WIFI_SCAN_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout (%d ms) durante la scansione WiFi - il tentativo continua in background",
                 WIFI_SCAN_TIMEOUT_MS);
        scan_ctx_release(ctx);
        return 0;
    }

    size_t n = ctx->count < max_results ? ctx->count : max_results;
    memcpy(out, ctx->out, n * sizeof(wifi_scan_result_t));
    scan_ctx_release(ctx);
    return n;
}
