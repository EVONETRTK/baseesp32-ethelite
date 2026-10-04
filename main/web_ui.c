#include "web_ui.h"
#include "version.h"
#include "settings.h"
#include "ota_update.h"
#include "sd_update.h"
#include "base_measure.h"
#include "time_sync.h"
#include "diag_log.h"
#include "data_usage.h"
#include "sim_plan.h"
#include "sim_tools.h"
#include "remote_status.h"
#include "online_update.h"
#include "status.h"
#include "log_buffer.h"
#include "sys_stats.h"
#include "config_backup.h"
#include "gnss_ubx_ack.h"
#include "vpn_link.h"
#include "raw_log.h"
#include "gnss_signal.h"
#include "rtcm3_stats.h"
#include "gnss_fix.h"
#include "wifi_link.h"
#include "eth_link.h"
#include "ntrip_rover_client.h"
#include "ntrip_client.h"
#include "cellular_link.h"
#include "alerts.h"
#include "base_monitor.h"
#include "ntrip_caster_server.h"
#include "geo_convert.h"
#include "ppp_log.h"
#include "fw_archive.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "web_ui";

// Protegge l'intera sequenza "leggi-modifica-scrivi" delle impostazioni,
// non solo le singole chiamate a settings_get()/settings_save() (gia'
// atomiche per conto proprio, vedi settings.c). Trovato con un utente che
// segnalava byte strani nell'elenco delle reti WiFi "conosciute": il test
// di connessione WiFi (wifi_test_connect_task, gira in background fino a
// 15s) e il salvataggio delle impostazioni generali (settings_post_handler,
// sincrono ma puo' partire IN QUALSIASI momento su un altro worker HTTP
// mentre il test e' ancora in corso) leggono ciascuno una propria
// istantanea della configurazione, la modificano per conto proprio, e la
// riscrivono per intero - se si sovrappongono, chi salva per ultimo
// sovrascrive il lavoro dell'altro con un'istantanea piu' vecchia. Preso
// prima di settings_get() e rilasciato dopo settings_save() in ognuno dei
// punti che fanno questa sequenza, cosi' non possono mai sovrapporsi.
static SemaphoreHandle_t s_settings_edit_mutex;

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t icon192_png_start[] asm("_binary_icon192_png_start");
extern const uint8_t icon192_png_end[]   asm("_binary_icon192_png_end");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");
// Scheda di accesso stampabile (web/access.html): indirizzi, utenti, password
// e istruzioni, generata dalla base con i dati del momento.
extern const uint8_t access_html_start[] asm("_binary_access_html_start");
extern const uint8_t access_html_end[]   asm("_binary_access_html_end");

// HTTP Basic Auth, utente fisso "admin" + codice impostabile dalla UI
// stessa (settings.admin_code). Un codice vuoto disabilita la protezione
// (usato anche come stato transitorio, es. subito dopo un flash pulito
// prima che venga letta la configurazione - non dovrebbe capitare visto
// che apply_defaults() imposta sempre un codice di default).
static bool check_auth(httpd_req_t *req)
{
    app_settings_t s = settings_get();
    if (strlen(s.admin_code) == 0) {
        return true;
    }

    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_hdr, "Basic ", 6) != 0) {
        return false;
    }

    unsigned char decoded[96];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *) auth_hdr + 6, strlen(auth_hdr + 6)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char expected[128];
    snprintf(expected, sizeof(expected), "admin:%s", s.admin_code);

    return strcmp((const char *) decoded, expected) == 0;
}

// Cookie di sessione: il popup Basic Auth del browser non viene ricordato
// in modo affidabile su tutti i browser/situazioni (es. passando tra
// l'IP dell'AP 192.168.4.1 e quello della rete di casa, il browser li
// tratta come siti diversi e chiede di nuovo le credenziali). Dopo un
// primo accesso riuscito via Basic Auth, impostiamo un cookie che vale
// da solo per ~30 giorni sullo stesso indirizzo, senza dover reinserire
// nulla.
#define AUTH_COOKIE_NAME "evonetrtk_auth"
#define AUTH_COOKIE_MAX_AGE_S (30 * 24 * 3600)

static bool check_auth_cookie(httpd_req_t *req, const app_settings_t *s)
{
    if (strlen(s->admin_code) == 0) {
        return true;
    }
    char cookie_hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) != ESP_OK) {
        return false;
    }
    char expected[160];
    snprintf(expected, sizeof(expected), AUTH_COOKIE_NAME "=%s", s->admin_code);
    return strstr(cookie_hdr, expected) != NULL;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    // Copre in un colpo solo tutte le richieste protette (ogni handler
    // eccetto index_get_handler chiama questa funzione per prima cosa):
    // conferma se una richiesta arriva davvero al dispositivo, cosa non
    // ovvia dal solo log applicativo dato che molti handler non
    // scrivono nulla quando vanno a buon fine.
    // Le GET arrivano ogni 3 s dal pannello aperto: a livello INFO
    // riempivano il log in memoria in un paio di minuti. Solo i comandi
    // (POST) restano visibili.
    if (req->method == HTTP_GET) {
        ESP_LOGD(TAG, "Richiesta %s %s", http_method_str(req->method), req->uri);
    } else {
        ESP_LOGI(TAG, "Richiesta %s %s", http_method_str(req->method), req->uri);
    }

    app_settings_t s = settings_get();

    if (check_auth_cookie(req, &s)) {
        return ESP_OK;
    }

    if (check_auth(req)) {
        // Autenticato via Basic Auth: imposta anche il cookie di sessione
        // cosi' le richieste successive non lo richiedono piu'. Buffer
        // "static": il server web qui gestisce una richiesta alla volta
        // (nessun worker parallelo configurato), il valore resta valido
        // fino a quando httpd_resp_send* viene chiamato piu' avanti nello
        // stesso handler che ha invocato questa funzione.
        static char cookie_val[192];
        snprintf(cookie_val, sizeof(cookie_val), AUTH_COOKIE_NAME "=%s; Max-Age=%d; Path=/",
                 s.admin_code, AUTH_COOKIE_MAX_AGE_S);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_val);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Autenticazione fallita per %s", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EVONETRTK\"");
    // Cookie con una password vecchia (cambiata dal pannello): lo si
    // cancella, altrimenti il browser continua a mandarlo (03/10/2026: dopo
    // il cambio di password il pannello non si apriva piu').
    char ck[8];
    if (httpd_req_get_hdr_value_str(req, "Cookie", ck, sizeof(ck)) != ESP_ERR_NOT_FOUND) {
        httpd_resp_set_hdr(req, "Set-Cookie", AUTH_COOKIE_NAME "=; Max-Age=0; Path=/");
    }
    // Il browser mostra questa pagina se si annulla la richiesta di
    // credenziali (prima era vuota, senza spiegazioni).
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_sendstr(req,
        "<!doctype html><html lang=\"it\"><head><meta charset=\"utf-8\">"
        "<meta name=\"viewport\" content=\"width=device-width,initial-scale=1\"><title>Accesso negato</title></head>"
        "<body style=\"font-family:system-ui,Arial,sans-serif;max-width:520px;margin:40px auto;padding:0 16px;line-height:1.5\">"
        "<h2>Accesso negato</h2>"
        "<p>Per entrare nel pannello servono:</p>"
        "<ul><li>nome utente: <b>admin</b> (sempre questo)</li>"
        "<li>password: quella impostata nella scheda Sicurezza (&quot;Codice di accesso&quot;)</li></ul>"
        "<p>Se la password e' stata cambiata da poco e il browser non la chiede piu': chiudere tutte le schede del pannello "
        "e aprirlo in una finestra in incognito, oppure cancellare i dati di questo sito nel browser.</p>"
        "<p>Password dimenticata: pulsante BOOT premuto 5 secondi (reset di fabbrica, password 1234; si perde la configurazione).</p>"
        "<p><a href=\"/\">Riprova</a></p></body></html>");
    return ESP_FAIL;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    // Senza questo header il browser puo' tenersi in cache la pagina per
    // giorni (nessun ETag/Last-Modified da confrontare, stesso indirizzo
    // ad ogni visita) - un aggiornamento firmware che cambia la pagina
    // web (icone, fix del campo password, qualunque cosa) rischiava di
    // non avere ALCUN effetto visibile finche' l'utente non svuotava la
    // cache a mano, pur avendo il nuovo codice gia' installato sul
    // dispositivo. Trovato dopo che piu' fix lato pagina web sembravano
    // "non fare niente" nonostante fossero installati e verificati.
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *) index_html_start, len);
}

static esp_err_t access_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    // EMBED_TXTFILES aggiunge uno zero finale: non va inviato.
    return httpd_resp_send(req, (const char *) access_html_start, access_html_end - access_html_start - 1);
}

typedef struct {
    const char *given;  // password admin reinserita
    bool ok;
    cJSON *out;
} access_secrets_ctx_t;

static void peek_access_secrets(const app_settings_t *s, void *arg)
{
    access_secrets_ctx_t *c = (access_secrets_ctx_t *) arg;
    c->ok = s->admin_code[0] == 0 || strcmp(c->given, s->admin_code) == 0;
    if (!c->ok) {
        return;
    }
    cJSON *o = c->out;
    cJSON_AddStringToObject(o, "admin_password", s->admin_code);
    cJSON_AddStringToObject(o, "ap_password", s->ap_password);
    cJSON_AddStringToObject(o, "wifi_password", s->wifi_password);
    cJSON_AddStringToObject(o, "ntrip_username", s->ntrip_username);
    cJSON_AddStringToObject(o, "ntrip_password", s->ntrip_password);
    cJSON_AddStringToObject(o, "rover_username", s->rover_username);
    cJSON_AddStringToObject(o, "rover_password", s->rover_password);
    cJSON_AddStringToObject(o, "caster_server_password", s->ntrip_caster_server_password);
    cJSON_AddStringToObject(o, "cellular_apn", s->cellular_apn);
    cJSON *nets = cJSON_AddArrayToObject(o, "wifi_networks");
    for (int i = 0; i < WIFI_KNOWN_NETWORKS_MAX; i++) {
        if (s->wifi_known_networks[i].ssid[0]) {
            cJSON *n = cJSON_CreateObject();
            cJSON_AddStringToObject(n, "ssid", s->wifi_known_networks[i].ssid);
            cJSON_AddStringToObject(n, "password", s->wifi_known_networks[i].password);
            cJSON_AddItemToArray(nets, n);
        }
    }
}

// Password per la scheda di accesso: solo con la password admin reinserita
// (non basta il cookie del pannello rimasto aperto su un PC qualunque).
// Nessun'altra risposta del pannello contiene password.
static esp_err_t access_secrets_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[128] = {0};
    int len = req->content_len < (int) sizeof(buf) - 1 ? req->content_len : (int) sizeof(buf) - 1;
    if (len <= 0 || httpd_req_recv(req, buf, len) != len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo mancante");
    }
    cJSON *root = cJSON_Parse(buf);
    const cJSON *p = root ? cJSON_GetObjectItemCaseSensitive(root, "password") : NULL;
    char given[40] = "";
    if (p && cJSON_IsString(p)) {
        strlcpy(given, p->valuestring, sizeof(given));
    }
    cJSON_Delete(root);
    access_secrets_ctx_t ctx = { .given = given, .ok = false, .out = cJSON_CreateObject() };
    settings_peek(peek_access_secrets, &ctx);
    memset(given, 0, sizeof(given));
    memset(buf, 0, sizeof(buf));
    if (!ctx.ok) {
        cJSON_Delete(ctx.out);
        ESP_LOGW(TAG, "Scheda di accesso: password admin errata");
        vTaskDelay(pdMS_TO_TICKS(1000)); // rallenta i tentativi a caso
        httpd_resp_set_status(req, "403 Forbidden");
        return httpd_resp_sendstr(req, "{\"error\":\"password errata\"}");
    }
    char *json = cJSON_PrintUnformatted(ctx.out);
    cJSON_Delete(ctx.out);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    ESP_LOGI(TAG, "Scheda di accesso: password mostrate");
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store");
    esp_err_t err = httpd_resp_sendstr(req, json);
    memset(json, 0, strlen(json));
    free(json);
    return err;
}

// Icona e "manifesto" per aggiungere il pannello alla schermata Home del
// telefono (si apre a schermo intero, come un'app). Senza autenticazione:
// non contengono nulla di riservato e il browser li chiede senza credenziali.
static esp_err_t icon_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "image/png");
    httpd_resp_set_hdr(req, "Cache-Control", "max-age=86400");
    return httpd_resp_send(req, (const char *) icon192_png_start, icon192_png_end - icon192_png_start);
}

static esp_err_t manifest_get_handler(httpd_req_t *req)
{
    httpd_resp_set_type(req, "application/manifest+json");
    return httpd_resp_sendstr(req, "{\"name\":\"EVONETRTK\",\"short_name\":\"EVONETRTK\",\"start_url\":\"/\",\"display\":\"standalone\",\"background_color\":\"#e8f0fe\",\"theme_color\":\"#2563eb\",\"icons\":[{\"src\":\"/icon192.png\",\"sizes\":\"192x192\",\"type\":\"image/png\",\"purpose\":\"any maskable\"}]}");
}

static const char *net_status_str(net_status_t s)
{
    switch (s) {
    case NET_STATUS_WIFI:     return "wifi";
    case NET_STATUS_CELLULAR: return "cellulare";
    default:                  return "nessuna";
    }
}

static const char *gnss_chip_str(gnss_chip_t c)
{
    switch (c) {
    case GNSS_CHIP_UNICORE: return "unicore";
    case GNSS_CHIP_LC29H:   return "lc29h";
    case GNSS_CHIP_BYNAV:   return "bynav";
    case GNSS_CHIP_BYNAV_M21D: return "bynav_m21d";
    case GNSS_CHIP_L76K:    return "l76k";
    default:                return "ublox";
    }
}

static const char *device_mode_str(device_mode_t m)
{
    return m == DEVICE_MODE_ROVER ? "rover" : "base";
}

static const char *oled_controller_str(oled_controller_t c)
{
    switch (c) {
    case OLED_CTRL_SH1106:  return "sh1106";
    case OLED_CTRL_SSD1309: return "ssd1309";
    default:                return "ssd1306";
    }
}

static const char *base_position_mode_str(base_position_mode_t m)
{
    return m == BASE_POSITION_MANUAL ? "manual" : "auto";
}

static const char *network_mode_str(network_mode_t m)
{
    switch (m) {
    case NETWORK_MODE_WIFI_ONLY:      return "wifi";
    case NETWORK_MODE_CELLULAR_ONLY:  return "cellular";
    case NETWORK_MODE_ETHERNET_ONLY:  return "ethernet_only";
    default:                          return "both";
    }
}

static const char *rgb_mode_str(rgb_led_mode_t m)
{
    switch (m) {
    case RGB_LED_WS2812: return "ws2812";
    case RGB_LED_PWM3:   return "pwm3";
    default:              return "none";
    }
}

// Il nome di una rete WiFi rilevata in scansione e' una sequenza di byte
// arbitraria (non tutti i router usano UTF-8 per nomi con caratteri
// speciali) - se contiene byte non validi come UTF-8, JSON/il browser li
// sostituiscono con punti interrogativi quando li mostrano. Il problema
// vero non e' solo estetico: se l'utente clicca su quel risultato per
// compilare il campo SSID, verrebbe salvato il nome CON i punti
// interrogativi al posto dei byte originali - una rete che di fatto non
// esiste, causa reale di "non riesco a collegarmi" nonostante password
// corretta (diagnosticato con l'utente via log seriale dal vivo).
//
// Fix: si trattano i byte grezzi dell'SSID come se fossero Latin-1
// (ISO-8859-1, un byte = un carattere) e si convertono in UTF-8 valido
// prima di mandarli al browser - qualunque sequenza di byte, in qualunque
// codifica fosse realmente, sopravvive cosi' intatta (nessun punto
// interrogativo, nessuna perdita di informazione) e il giro inverso
// (safe_utf8_to_raw_ssid_bytes, usato quando l'utente si collega/salva)
// la riporta esattamente ai byte originali - la connessione usera' quindi
// sempre il nome VERO della rete, byte per byte, anche per nomi con
// caratteri speciali che il browser da solo mostrerebbe in modo diverso.
// Usata anche per il wifi_ssid gia' salvato restituito da /api/status
// (potrebbe contenere byte non-UTF8 se salvato prima di questo fix).
static void raw_ssid_bytes_to_safe_utf8(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (const unsigned char *p = (const unsigned char *) in; *p && o + 2 < out_size; p++) {
        if (*p < 0x80) {
            out[o++] = (char) *p;
        } else {
            out[o++] = (char) (0xC0 | (*p >> 6));
            out[o++] = (char) (0x80 | (*p & 0x3F));
        }
    }
    out[o] = '\0';
}

// Inversa di raw_ssid_bytes_to_safe_utf8() - vedi commento sopra. Una
// sequenza inattesa (non prodotta da quella funzione, es. un carattere
// speciale digitato a mano dall'utente invece che selezionato da una
// scansione) viene copiata byte per byte invece di bloccare il
// salvataggio: nel peggiore dei casi un carattere raro digitato a mano
// non sopravvive esattamente, ma la funzione non fallisce mai.
static void safe_utf8_to_raw_ssid_bytes(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    const unsigned char *p = (const unsigned char *) in;
    while (*p && o + 1 < out_size) {
        if (*p < 0x80) {
            out[o++] = (char) *p;
            p++;
        } else if ((*p & 0xE0) == 0xC0 && p[1] != '\0' && (p[1] & 0xC0) == 0x80) {
            unsigned int cp = ((unsigned int) (*p & 0x1F) << 6) | (p[1] & 0x3F);
            out[o++] = (char) (cp & 0xFF);
            p += 2;
        } else {
            out[o++] = (char) *p;
            p++;
        }
    }
    out[o] = '\0';
}

// Invia la parte gia' costruita della risposta JSON (senza le graffe esterne,
// con la virgola di separazione) e ricomincia con un oggetto vuoto: cosi' in
// memoria c'e' sempre solo un pezzo della risposta di stato.
static void status_flush(httpd_req_t *req, cJSON **root, bool *first)
{
    char *part = *root ? cJSON_PrintUnformatted(*root) : NULL;
    cJSON_Delete(*root);
    *root = cJSON_CreateObject();
    if (!part) {
        return;
    }
    size_t len = strlen(part);
    if (len > 2) {
        part[len - 1] = '\0';            // toglie la graffa finale
        part[0] = *first ? '{' : ',';    // la prima parte apre l'oggetto
        httpd_resp_sendstr_chunk(req, part);
        *first = false;
    }
    free(part);
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    app_settings_t s = settings_get();

    // Risposta inviata a pezzi (status_flush): costruita tutta in memoria
    // occupava ~20 KB a richiesta, e con il pannello aperto la memoria
    // libera minima e' scesa a 2,8 KB (03/10/2026).
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    bool first = true;
    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "net", net_status_str(status_get_net()));
    {
        // Nome REALE della rete a cui si e' effettivamente associati ora
        // (dal driver, non dalle impostazioni salvate) - puo' differire
        // dalla rete "principale" se ci si e' collegati a una rete
        // conosciuta diversa (vedi wifi_link_connect_known()). Mostrato
        // nella pagina Stato cosi' si vede con certezza a quale rete si
        // e' davvero collegati, non solo "WiFi" in generale.
        char current_ssid[33];
        if (wifi_link_get_current_ssid(current_ssid, sizeof(current_ssid))) {
            char safe_current_ssid[65];
            raw_ssid_bytes_to_safe_utf8(current_ssid, safe_current_ssid, sizeof(safe_current_ssid));
            cJSON_AddStringToObject(root, "wifi_current_ssid", safe_current_ssid);
        }
    }
    {
        // IP reale attuale delle interfacce di rete (STA + AP), letto dal
        // driver, non dalle impostazioni: e' cambiato piu' volte durante lo
        // sviluppo (router diverso, DHCP diverso) causando NTRIP rotto per
        // host non piu' raggiungibile senza che si notasse subito - va
        // mostrato sempre in "Stato" per essere visibile a colpo d'occhio.
        esp_netif_ip_info_t ip_info;
        esp_netif_t *sta_netif = wifi_link_get_sta_netif();
        if (sta_netif && esp_netif_get_ip_info(sta_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(root, "wifi_ip", ip_str);
        }
        esp_netif_t *ap_netif = wifi_link_get_ap_netif();
        if (ap_netif && esp_netif_get_ip_info(ap_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(root, "ap_ip", ip_str);
        }
        // Ethernet e' indipendente da WiFi/cellulare (vedi net_manager.c) e
        // fino ad ora non aveva nessuna visibilita' in UI - eth_link_*()
        // sono gia' no-op sicuri se BASEESP32_ETHERNET_ENABLE e' disattivato
        // in questo build (eth_connected resta false, eth_netif NULL).
        cJSON_AddBoolToObject(root, "eth_connected", eth_link_is_connected());
        esp_netif_t *eth_netif = eth_link_get_netif();
        if (eth_netif && esp_netif_get_ip_info(eth_netif, &ip_info) == ESP_OK && ip_info.ip.addr != 0) {
            char ip_str[16];
            esp_ip4addr_ntoa(&ip_info.ip, ip_str, sizeof(ip_str));
            cJSON_AddStringToObject(root, "eth_ip", ip_str);
        }
    }
    cJSON_AddNumberToObject(root, "rtcm_bytes", status_get_rtcm_total_bytes());
    cJSON_AddNumberToObject(root, "last_rtcm_us", (double) status_get_last_rtcm_time_us());
    cJSON_AddNumberToObject(root, "last_gga_sent_us", (double) status_get_last_gga_sent_time_us());

    ntrip_conn_status_t ntrip = status_ntrip_get();
    cJSON_AddBoolToObject(root, "ntrip_connected", ntrip.connected);
    cJSON_AddNumberToObject(root, "ntrip_connected_since_us", (double) ntrip.connected_since_us);
    cJSON_AddNumberToObject(root, "ntrip_last_disconnect_us", (double) ntrip.last_disconnect_us);
    cJSON_AddNumberToObject(root, "ntrip_connect_count", ntrip.connect_count);
    cJSON_AddStringToObject(root, "ntrip_last_error", ntrip.last_error);
    status_flush(req, &root, &first); // invia questa parte e libera la memoria

    // Tempo del dispositivo (dal boot, stessa base di ntrip_connected_since_us
    // e last_disconnect_us) al momento di generare questa risposta - permette
    // al browser di calcolare "da quanto" senza affidarsi al proprio
    // orologio (che non ha comunque relazione con l'uptime del dispositivo).
    {
        char now[24];
        time_sync_format_now(now, sizeof(now));
        if (now[0]) {
            cJSON_AddStringToObject(root, "local_time", now); // assente finche' l'NTP non sincronizza
        }
    }
    cJSON_AddNumberToObject(root, "now_us", (double) esp_timer_get_time());
    cJSON_AddNumberToObject(root, "last_online_update_check_us", (double) status_get_last_online_update_check_us());
    cJSON_AddStringToObject(root, "gnss_chip", gnss_chip_str(s.gnss_chip));
    cJSON_AddNumberToObject(root, "bynav_ant1_x_m", s.bynav_ant1_x_m);
    cJSON_AddNumberToObject(root, "bynav_ant1_y_m", s.bynav_ant1_y_m);
    cJSON_AddNumberToObject(root, "bynav_ant1_z_m", s.bynav_ant1_z_m);
    cJSON_AddNumberToObject(root, "bynav_ant2_x_m", s.bynav_ant2_x_m);
    cJSON_AddNumberToObject(root, "bynav_ant2_y_m", s.bynav_ant2_y_m);
    cJSON_AddNumberToObject(root, "bynav_ant2_z_m", s.bynav_ant2_z_m);
    cJSON_AddNumberToObject(root, "bynav_rbv_roll_deg", s.bynav_rbv_roll_deg);
    cJSON_AddNumberToObject(root, "bynav_rbv_pitch_deg", s.bynav_rbv_pitch_deg);
    cJSON_AddNumberToObject(root, "bynav_rbv_yaw_deg", s.bynav_rbv_yaw_deg);
    if (s.gnss_chip == GNSS_CHIP_BYNAV_M21D && s.device_mode == DEVICE_MODE_ROVER) {
        bynav_ins_status_t ins = status_bynav_ins_get();
        cJSON_AddBoolToObject(root, "bynav_ins_have_attitude", ins.have_attitude);
        if (ins.have_attitude) {
            cJSON_AddStringToObject(root, "bynav_ins_status", ins.ins_status);
            cJSON_AddNumberToObject(root, "bynav_ins_heading_deg", ins.heading_deg);
            cJSON_AddNumberToObject(root, "bynav_ins_pitch_deg", ins.pitch_deg);
            cJSON_AddNumberToObject(root, "bynav_ins_roll_deg", ins.roll_deg);
        }
    }
    cJSON_AddBoolToObject(root, "rtcm_1005_enable", s.rtcm_1005_enable);
    cJSON_AddBoolToObject(root, "rtcm_1230_enable", s.rtcm_1230_enable);
    cJSON_AddBoolToObject(root, "rtcm_1007_enable", s.rtcm_1007_enable);
    cJSON_AddBoolToObject(root, "rtcm_1008_enable", s.rtcm_1008_enable);
    cJSON_AddBoolToObject(root, "rtcm_1019_enable", s.rtcm_1019_enable);
    cJSON_AddBoolToObject(root, "rtcm_1020_enable", s.rtcm_1020_enable);
    cJSON_AddBoolToObject(root, "rtcm_1074_enable", s.rtcm_1074_enable);
    cJSON_AddBoolToObject(root, "rtcm_1077_enable", s.rtcm_1077_enable);
    cJSON_AddBoolToObject(root, "rtcm_1084_enable", s.rtcm_1084_enable);
    cJSON_AddBoolToObject(root, "rtcm_1087_enable", s.rtcm_1087_enable);
    cJSON_AddBoolToObject(root, "rtcm_1094_enable", s.rtcm_1094_enable);
    cJSON_AddBoolToObject(root, "rtcm_1097_enable", s.rtcm_1097_enable);
    cJSON_AddBoolToObject(root, "rtcm_1124_enable", s.rtcm_1124_enable);
    cJSON_AddBoolToObject(root, "rtcm_1127_enable", s.rtcm_1127_enable);
    cJSON_AddStringToObject(root, "device_mode", device_mode_str(s.device_mode));
    cJSON_AddStringToObject(root, "active_device_mode", status_get_active_rover() ? "rover" : "base");
    cJSON_AddStringToObject(root, "network_mode", network_mode_str(s.network_mode));
    {
        char safe_wifi_ssid[65];
        raw_ssid_bytes_to_safe_utf8(s.wifi_ssid, safe_wifi_ssid, sizeof(safe_wifi_ssid));
        cJSON_AddStringToObject(root, "wifi_ssid", safe_wifi_ssid);
    }
    {
        // Solo i nomi (mai le password) delle altre reti "conosciute" -
        // vedi settings.h/app_settings_remember_wifi() - per far vedere
        // nella UI che il dispositivo le ricorda davvero.
        cJSON *known = cJSON_CreateArray();
        for (int i = 0; i < WIFI_KNOWN_NETWORKS_MAX; i++) {
            if (s.wifi_known_networks[i].ssid[0] == '\0') {
                continue;
            }
            char safe_known_ssid[65];
            raw_ssid_bytes_to_safe_utf8(s.wifi_known_networks[i].ssid, safe_known_ssid, sizeof(safe_known_ssid));
            cJSON_AddItemToArray(known, cJSON_CreateString(safe_known_ssid));
        }
        cJSON_AddItemToObject(root, "wifi_known_networks", known);
    }
    cJSON_AddStringToObject(root, "cellular_apn", s.cellular_apn);
    cJSON_AddBoolToObject(root, "cellular_is_sim868", s.cellular_is_sim868);
    status_flush(req, &root, &first); // invia questa parte e libera la memoria

    // Scelta unica per il pannello: modulo LilyGO, SIMCom originale o SIM868.
    cJSON_AddStringToObject(root, "cellular_module", s.cellular_is_sim868 ? "sim868"
                            : (s.cellular_simcom_std ? "sim7600_simcom" : "sim7600_lilygo"));
    cJSON_AddStringToObject(root, "ntrip_host", s.ntrip_host);
    cJSON_AddNumberToObject(root, "ntrip_port", s.ntrip_port);
    cJSON_AddStringToObject(root, "ntrip_mountpoint", s.ntrip_mountpoint);
    // Credenziali del rover separate da quelle della base (1.19.66): la
    // password non viene mai restituita, come le altre.
    cJSON_AddStringToObject(root, "rover_mountpoint", s.rover_mountpoint);
    cJSON_AddStringToObject(root, "rover_username", s.rover_username);
    cJSON_AddStringToObject(root, "ap_ssid", s.ap_ssid);
    cJSON_AddStringToObject(root, "device_serial", s.device_serial);
    {
        // Permette alla UI di segnalare quando la matricola e' stata forzata
        // a un valore diverso da quello ricavato dal MAC di questo chip
        // fisico (es. dispositivo sostituito ma matricola lasciata uguale
        // per continuita' di tracciamento, o errore di battitura).
        char mac_serial[7];
        settings_device_serial_from_mac(mac_serial, sizeof(mac_serial));
        cJSON_AddStringToObject(root, "device_serial_from_mac", mac_serial);
    }
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "firmware_release_notes", FIRMWARE_RELEASE_NOTES);
    cJSON_AddStringToObject(root, "ota_update_url", s.ota_update_url);
    cJSON_AddBoolToObject(root, "auto_update_check_enable", s.auto_update_check_enable);
    cJSON_AddNumberToObject(root, "auto_update_check_interval_h", s.auto_update_check_interval_h);
    cJSON_AddNumberToObject(root, "nmea_udp_port", s.nmea_udp_port);

    cJSON_AddBoolToObject(root, "alert_enable", s.alert_enable);
    cJSON_AddNumberToObject(root, "alert_threshold_min", s.alert_threshold_min);
    cJSON_AddStringToObject(root, "last_reset", sys_stats_last_reset());
    {
        // Ricevitore GNSS (UBX-MON-SYS): temperatura e stato interno.
        gnss_sys_status_t gs = status_gnss_sys_get();
        if (gs.have) {
            cJSON_AddNumberToObject(root, "gnss_temp_c", gs.temp_c);
            cJSON_AddNumberToObject(root, "gnss_temp_max_c", gs.temp_max_c);
            cJSON_AddNumberToObject(root, "gnss_cpu_load", gs.cpu_load);
            cJSON_AddNumberToObject(root, "gnss_mem_usage", gs.mem_usage);
            cJSON_AddNumberToObject(root, "gnss_run_time_s", gs.run_time_s);
            cJSON_AddNumberToObject(root, "gnss_warnings", gs.warnings);
            cJSON_AddNumberToObject(root, "gnss_errors", gs.errors);
        }
    }
    {
        // Gestione della SIM: impostazioni del credito e ultimo esito.
        cJSON_AddStringToObject(root, "sim_operator", s.sim_operator);
        cJSON_AddStringToObject(root, "remote_url", s.remote_url);
        cJSON_AddBoolToObject(root, "vpn_enable", s.vpn_enable);
        cJSON_AddStringToObject(root, "vpn_address", s.vpn_address);
        cJSON_AddStringToObject(root, "vpn_peer_public_key", s.vpn_peer_public_key);
        cJSON_AddStringToObject(root, "vpn_endpoint", s.vpn_endpoint);
        cJSON_AddNumberToObject(root, "vpn_port", s.vpn_port ? s.vpn_port : 51820);
        cJSON_AddBoolToObject(root, "vpn_has_key", s.vpn_private_key[0] != 0);
        cJSON_AddNumberToObject(root, "remote_interval_min", s.remote_interval_min);
        cJSON_AddStringToObject(root, "remote_last", remote_status_last_result());
        cJSON_AddStringToObject(root, "sim_credit_mode", s.sim_credit_mode == 1 ? "sms" : "ussd");
        cJSON_AddStringToObject(root, "sim_credit_code", s.sim_credit_code);
        cJSON_AddStringToObject(root, "sim_credit_sms_number", s.sim_credit_sms_number);
        cJSON_AddStringToObject(root, "sim_credit_sms_text", s.sim_credit_sms_text);
        cJSON_AddNumberToObject(root, "sim_credit_every_days", s.sim_credit_every_days);
        cJSON_AddNumberToObject(root, "sim_credit_hour", s.sim_credit_hour);
        cJSON_AddNumberToObject(root, "sim_credit_min_eur", s.sim_credit_min_eur);
        static sim_tools_status_t st; // statico: ~400 byte, fuori dallo stack del server
        sim_tools_get_status(&st);
        cJSON_AddBoolToObject(root, "sim_busy", st.busy);
        cJSON_AddStringToObject(root, "sim_last_msg", st.last_msg);
        cJSON_AddStringToObject(root, "sim_iccid", st.iccid);
        if (st.credit_at > 0) {
            char when[24];
            struct tm tm;
            localtime_r(&st.credit_at, &tm);
            strftime(when, sizeof(when), "%Y-%m-%d %H:%M", &tm);
            cJSON_AddStringToObject(root, "sim_credit_at", when);
            cJSON_AddStringToObject(root, "sim_credit_text", st.credit_text);
            if (st.credit_eur_valid) {
                cJSON_AddNumberToObject(root, "sim_credit_eur", st.credit_eur);
            }
        }
    }
    cJSON_AddStringToObject(root, "fw_rollback_note", ota_update_rollback_note());
    cJSON_AddStringToObject(root, "last_crash", sys_stats_last_crash());
    cJSON_AddStringToObject(root, "config_guard_note", config_backup_guard_note());
    cJSON_AddStringToObject(root, "gnss_model", gnss_ubx_ack_model());
    cJSON_AddStringToObject(root, "gnss_fw", gnss_ubx_ack_fw());
    {
        vpn_status_t vs;
        vpn_link_get_status(&vs);
        cJSON_AddBoolToObject(root, "vpn_up", vs.up);
        cJSON_AddBoolToObject(root, "vpn_started", vs.started);
        cJSON_AddStringToObject(root, "vpn_public_key", vs.public_key);
        cJSON_AddStringToObject(root, "vpn_note", vs.last_error);
        if (vs.up) {
            cJSON_AddNumberToObject(root, "vpn_up_s", (double) ((esp_timer_get_time() - vs.up_since_us) / 1000000));
        }
    }
    {
        raw_log_status_t rs;
        raw_log_get_status(&rs);
        cJSON_AddBoolToObject(root, "raw_active", rs.active);
        cJSON_AddBoolToObject(root, "raw_waiting_time", rs.waiting_time);
        cJSON_AddStringToObject(root, "raw_file", rs.file);
        cJSON_AddNumberToObject(root, "raw_bytes", (double) rs.bytes);
        cJSON_AddNumberToObject(root, "raw_frames", rs.frames);
        cJSON_AddNumberToObject(root, "raw_dropped", rs.dropped);
        cJSON_AddNumberToObject(root, "raw_end_unix", (double) rs.end_unix);
        cJSON_AddNumberToObject(root, "raw_interval_s", rs.interval_s);
        cJSON_AddStringToObject(root, "raw_note", rs.note);
    }
    cJSON_AddBoolToObject(root, "fw_pending", ota_update_is_pending());
    {
        // Traffico stimato (data_usage.c) e piano dati della SIM.
        data_usage_t du = data_usage_get();
        cJSON_AddStringToObject(root, "data_month", du.month);
        cJSON_AddNumberToObject(root, "data_cell_month_bytes", (double) du.cell_month);
        cJSON_AddNumberToObject(root, "data_cell_day_bytes", (double) du.cell_day);
        cJSON_AddNumberToObject(root, "data_wifi_month_bytes", (double) du.wifi_month);
        cJSON_AddNumberToObject(root, "data_wifi_day_bytes", (double) du.wifi_day);
        cJSON_AddNumberToObject(root, "data_plan_mb", s.data_plan_mb);
        cJSON_AddNumberToObject(root, "data_counted_s", du.counted_s);
        cJSON_AddNumberToObject(root, "data_period_days", du.period_days);
    }
    {
        // Rinnovo del piano della SIM (sim_plan.c).
        char d[16] = "";
        if (s.sim_renew_date) {
            snprintf(d, sizeof(d), "%04u-%02u-%02u", (unsigned) (s.sim_renew_date / 10000),
                     (unsigned) (s.sim_renew_date / 100 % 100), (unsigned) (s.sim_renew_date % 100));
        }
        cJSON_AddStringToObject(root, "sim_renew_date", d);
        cJSON_AddStringToObject(root, "sim_renew_mode", s.sim_renew_mode == SIM_RENEW_MONTHLY ? "monthly"
                                : (s.sim_renew_mode == SIM_RENEW_DAYS ? "days" : "single"));
        cJSON_AddNumberToObject(root, "sim_renew_every_days", s.sim_renew_every_days);
        cJSON_AddNumberToObject(root, "sim_notice1_days", s.sim_notice1_days);
        cJSON_AddNumberToObject(root, "sim_notice2_days", s.sim_notice2_days);
        int left;
        if (sim_plan_days_left(&left)) {
            cJSON_AddNumberToObject(root, "sim_renew_days_left", left);
        }
        data_usage_day_t hist[DATA_USAGE_HISTORY_DAYS];
        int hn = data_usage_get_history(hist, DATA_USAGE_HISTORY_DAYS);
        cJSON *harr = cJSON_AddArrayToObject(root, "data_history");
        for (int i = 0; i < hn; i++) {
            cJSON *o = cJSON_CreateObject();
            char d[16];
            snprintf(d, sizeof(d), "%04u-%02u-%02u", (unsigned) (hist[i].day / 10000),
                     (unsigned) (hist[i].day / 100 % 100), (unsigned) (hist[i].day % 100));
            cJSON_AddStringToObject(o, "d", d);
            cJSON_AddNumberToObject(o, "c", (double) hist[i].cell_kb * 1000);
            cJSON_AddNumberToObject(o, "w", (double) hist[i].wifi_kb * 1000);
            cJSON_AddItemToArray(harr, o);
        }
    }
    {
        // Ultime cadute della connessione al caster, dalla piu' recente.
        ntrip_outage_t out[NTRIP_OUTAGE_LOG_LEN];
        int n = status_ntrip_get_outages(out, NTRIP_OUTAGE_LOG_LEN);
        cJSON *arr = cJSON_AddArrayToObject(root, "ntrip_outages");
        int64_t now = esp_timer_get_time();
        for (int i = 0; i < n; i++) {
            cJSON *o = cJSON_CreateObject();
            char when[24];
            time_sync_format_uptime(out[i].start_us, when, sizeof(when));
            cJSON_AddStringToObject(o, "at", when);
            cJSON_AddNumberToObject(o, "ago_s", (double) ((now - out[i].start_us) / 1000000));
            cJSON_AddNumberToObject(o, "duration_s", (double) (((out[i].end_us ? out[i].end_us : now) - out[i].start_us) / 1000000));
            cJSON_AddBoolToObject(o, "ongoing", out[i].end_us == 0);
            cJSON_AddStringToObject(o, "reason", out[i].reason);
            cJSON_AddItemToArray(arr, o);
        }
    }
    cJSON_AddStringToObject(root, "alert_smtp_host", s.alert_smtp_host);
    cJSON_AddNumberToObject(root, "alert_smtp_port", s.alert_smtp_port);
    cJSON_AddStringToObject(root, "alert_smtp_user", s.alert_smtp_user);
    cJSON_AddStringToObject(root, "alert_email_to", s.alert_email_to);
    cJSON_AddStringToObject(root, "alert_whatsapp_phone", s.alert_whatsapp_phone);
    status_flush(req, &root, &first); // invia questa parte e libera la memoria

    // alert_smtp_password e alert_whatsapp_apikey non vengono mai
    // restituiti (come ntrip_password sopra) - solo scrivibili dalla UI,
    // mai riletti.

    cJSON_AddBoolToObject(root, "base_drift_alert_enable", s.base_drift_alert_enable);
    cJSON_AddNumberToObject(root, "base_drift_threshold_m", s.base_drift_threshold_m);
    base_monitor_status_t drift = base_monitor_get_status();
    cJSON_AddBoolToObject(root, "base_drift_baseline_set", drift.baseline_set);
    if (drift.baseline_set) {
        cJSON_AddNumberToObject(root, "base_drift_m", drift.drift_m);
    }

    cJSON_AddStringToObject(root, "base_position_mode", base_position_mode_str(s.base_position_mode));
    cJSON_AddNumberToObject(root, "base_fixed_lat_deg", s.base_fixed_lat_deg);
    cJSON_AddNumberToObject(root, "base_fixed_lon_deg", s.base_fixed_lon_deg);
    cJSON_AddNumberToObject(root, "base_fixed_height_m", s.base_fixed_height_m);
    cJSON_AddNumberToObject(root, "base_svin_min_dur_s", s.base_svin_min_dur_s);
    cJSON_AddNumberToObject(root, "base_svin_acc_m", s.base_svin_acc_m);
    {
        // Stato del survey-in letto dal ricevitore u-blox (solo base, vedi
        // svin_poll_task in gnss_ubx.c). Assente se mai ricevuto.
        svin_status_t sv = status_svin_get();
        if (sv.have) {
            cJSON_AddBoolToObject(root, "svin_active", sv.active);
            cJSON_AddBoolToObject(root, "svin_valid", sv.valid);
            cJSON_AddNumberToObject(root, "svin_duration_s", sv.duration_s);
            cJSON_AddNumberToObject(root, "svin_mean_acc_m", sv.mean_acc_m);
        }
    }
    status_flush(req, &root, &first); // invia questa parte e libera la memoria

    // Ultima posizione rilevata dal ricevitore (ECEF, dallo stesso stream
    // RTCM 1005/1006 usato sopra per il rilevamento spostamenti) convertita
    // in lat/lon/quota - proposta dalla UI come default quando si passa a
    // posizione manuale, cosi' di norma basta confermare invece di doverla
    // trascrivere a mano da un'altra fonte (es. il display di un altro
    // ricevitore, o un servizio PPP).
    cJSON_AddBoolToObject(root, "base_current_position_set", drift.last_position_set);
    {
        base_measure_progress_t mp = base_measure_get_progress();
        cJSON_AddBoolToObject(root, "base_measure_active", mp.active);
        cJSON_AddStringToObject(root, "base_measure_msg", s.base_measure_msg);
        if (mp.active) {
            cJSON_AddNumberToObject(root, "base_measure_elapsed_s", mp.elapsed_s);
            cJSON_AddNumberToObject(root, "base_measure_fixed_n", mp.fixed_n);
            cJSON_AddNumberToObject(root, "base_measure_float_n", mp.float_n);
            cJSON_AddNumberToObject(root, "base_measure_target_n", mp.target_n);
            cJSON_AddNumberToObject(root, "base_measure_timeout_s", mp.timeout_s);
            cJSON_AddNumberToObject(root, "base_measure_quality", mp.quality);
        }
    }
    // Secondi dall'ultimo 1005/1006 inviato (-1 = mai dall'avvio).
    cJSON_AddNumberToObject(root, "base_position_age_s", drift.last_position_us > 0
        ? (double) ((esp_timer_get_time() - drift.last_position_us) / 1000000) : -1.0);
    if (drift.last_position_set) {
        double lat, lon, height;
        geo_ecef_to_llh(drift.last_ecef_x_m, drift.last_ecef_y_m, drift.last_ecef_z_m, &lat, &lon, &height);
        cJSON_AddNumberToObject(root, "base_current_lat_deg", lat);
        cJSON_AddNumberToObject(root, "base_current_lon_deg", lon);
        cJSON_AddNumberToObject(root, "base_current_height_m", height);
    }

    ppp_log_status_t ppp = ppp_log_get_status();
    cJSON_AddBoolToObject(root, "ppp_log_recording", ppp.recording);
    cJSON_AddBoolToObject(root, "ppp_log_file_exists", ppp.file_exists);
    if (ppp.recording || ppp.file_exists) {
        cJSON_AddNumberToObject(root, "ppp_log_bytes", (double) ppp.bytes_written);
        cJSON_AddNumberToObject(root, "ppp_log_started_at_us", (double) ppp.started_at_us);
    }

    cJSON_AddBoolToObject(root, "ntrip_caster_server_enable", s.ntrip_caster_server_enable);
    cJSON_AddNumberToObject(root, "ntrip_caster_server_port", s.ntrip_caster_server_port);
    cJSON_AddStringToObject(root, "ntrip_caster_server_mountpoint", s.ntrip_caster_server_mountpoint);
    cJSON_AddStringToObject(root, "ntrip_caster_server_username", s.ntrip_caster_server_username);
    cJSON_AddNumberToObject(root, "ntrip_caster_server_clients", (double) ntrip_caster_server_get_client_count());
    status_flush(req, &root, &first); // invia questa parte e libera la memoria

    // ntrip_caster_server_password non viene mai restituita (come le altre
    // password sopra) - solo scrivibile dalla UI, mai riletta.

    sd_update_status_t sd_status = sd_update_get_status();
    cJSON_AddBoolToObject(root, "sd_checked", sd_status.checked);
    if (sd_status.checked) {
        cJSON_AddBoolToObject(root, "sd_card_present", sd_status.card_present);
        cJSON_AddStringToObject(root, "sd_message", sd_status.message);
        if (sd_status.card_present) {
            cJSON_AddNumberToObject(root, "sd_total_bytes", (double) sd_status.total_bytes);
            cJSON_AddNumberToObject(root, "sd_used_bytes", (double) sd_status.used_bytes);
        }
    }

    status_flush(req, &root, &first); // invia questa parte e libera la memoria

    // Bluetooth Classic (SPP) non disponibile su ESP32-S3 (solo BLE, non
    // implementata su questa scheda) - i campi bt_* non vengono inviati.

    cJSON_AddNumberToObject(root, "gnss_uart_num", s.gnss_uart_num);
    cJSON_AddNumberToObject(root, "gnss_uart_tx_pin", s.gnss_uart_tx_pin);
    cJSON_AddNumberToObject(root, "gnss_uart_rx_pin", s.gnss_uart_rx_pin);
    cJSON_AddNumberToObject(root, "gnss_uart_baud", s.gnss_uart_baud);
    cJSON_AddStringToObject(root, "gnss_link", s.gnss_i2c ? "i2c" : "uart");

    cJSON_AddStringToObject(root, "rgb_led_mode", rgb_mode_str(s.rgb_led_mode));
    cJSON_AddNumberToObject(root, "rgb_led_ws2812_pin", s.rgb_led_ws2812_pin);
    cJSON_AddNumberToObject(root, "rgb_led_pwm_r_pin", s.rgb_led_pwm_r_pin);
    cJSON_AddNumberToObject(root, "rgb_led_pwm_g_pin", s.rgb_led_pwm_g_pin);
    cJSON_AddNumberToObject(root, "rgb_led_pwm_b_pin", s.rgb_led_pwm_b_pin);
    cJSON_AddBoolToObject(root, "rgb_led_pwm_active_low", s.rgb_led_pwm_active_low);

    cJSON_AddNumberToObject(root, "oled_sda_pin", s.oled_sda_pin);
    cJSON_AddNumberToObject(root, "oled_scl_pin", s.oled_scl_pin);
    cJSON_AddNumberToObject(root, "oled_i2c_addr", s.oled_i2c_addr);
    cJSON_AddStringToObject(root, "oled_controller", oled_controller_str(s.oled_controller));
    cJSON_AddBoolToObject(root, "oled_flip_h", s.oled_flip_h);
    cJSON_AddBoolToObject(root, "oled_flip_v", s.oled_flip_v);

    sys_stats_t stats = sys_stats_get();
    cJSON_AddNumberToObject(root, "free_heap_bytes", stats.free_heap_bytes);
    cJSON_AddNumberToObject(root, "min_free_heap_bytes", stats.min_free_heap_bytes);
    cJSON_AddNumberToObject(root, "total_heap_bytes", stats.total_heap_bytes);
    cJSON_AddBoolToObject(root, "psram_present", stats.psram_present);
    if (stats.psram_present) {
        cJSON_AddNumberToObject(root, "free_psram_bytes", stats.free_psram_bytes);
        cJSON_AddNumberToObject(root, "total_psram_bytes", stats.total_psram_bytes);
    }
    if (stats.cpu0_percent >= 0) {
        cJSON_AddNumberToObject(root, "cpu0_percent", stats.cpu0_percent);
    }
    if (stats.cpu1_percent >= 0) {
        cJSON_AddNumberToObject(root, "cpu1_percent", stats.cpu1_percent);
    }
    if (stats.chip_temp_c > -1000) {
        cJSON_AddNumberToObject(root, "chip_temp_c", stats.chip_temp_c);
    }
    if (stats.chip_temp_max_c > -1000) {
        cJSON_AddNumberToObject(root, "chip_temp_max_c", stats.chip_temp_max_c);
        // Quando: ora locale se l'NTP ha sincronizzato, altrimenti secondi fa.
        char when[24];
        time_sync_format_uptime(stats.chip_temp_max_us, when, sizeof(when));
        if (when[0]) {
            cJSON_AddStringToObject(root, "chip_temp_max_at", when);
        }
        cJSON_AddNumberToObject(root, "chip_temp_max_ago_s", (double) ((esp_timer_get_time() - stats.chip_temp_max_us) / 1000000));
    }

    status_flush(req, &root, &first);
    cJSON_Delete(root);
    httpd_resp_sendstr_chunk(req, first ? "{}" : "}");
    return httpd_resp_send_chunk(req, NULL, 0);
}

static esp_err_t signals_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();

    cJSON *sats = cJSON_CreateArray();
    gnss_sat_signal_t sat_list[GNSS_SIGNAL_MAX_SATS];
    size_t n = gnss_signal_get_satellites(sat_list, GNSS_SIGNAL_MAX_SATS);
    for (size_t i = 0; i < n; i++) {
        cJSON *sat = cJSON_CreateObject();
        cJSON_AddStringToObject(sat, "constellation", sat_list[i].constellation);
        cJSON_AddNumberToObject(sat, "prn", sat_list[i].prn);
        cJSON_AddNumberToObject(sat, "snr", sat_list[i].snr);
        cJSON_AddItemToArray(sats, sat);
    }
    cJSON_AddItemToObject(root, "satellites", sats);

    {
        // Byte totali visti dal boot per ciascun numero di messaggio RTCM3
        // (solo base - vedi rtcm3_stats.c) - permette alla UI di mostrare
        // quanto pesa davvero ciascun messaggio scelto in GNSS & NTRIP,
        // invece di doverlo indovinare.
        rtcm3_stat_entry_t entries[24];
        size_t n_stats = rtcm3_stats_get(entries, sizeof(entries) / sizeof(entries[0]));
        cJSON *rtcm_by_type = cJSON_CreateArray();
        for (size_t i = 0; i < n_stats; i++) {
            cJSON *e = cJSON_CreateObject();
            cJSON_AddNumberToObject(e, "type", entries[i].msg_type);
            cJSON_AddNumberToObject(e, "bytes", entries[i].bytes);
            cJSON_AddItemToArray(rtcm_by_type, e);
        }
        cJSON_AddItemToObject(root, "rtcm_bytes_by_type", rtcm_by_type);
    }

    gnss_fix_status_t fix = gnss_fix_get_status();
    cJSON_AddBoolToObject(root, "gnss_fix_valid", fix.valid);
    if (fix.valid) {
        cJSON_AddNumberToObject(root, "gnss_fix_quality", fix.quality);
        cJSON_AddStringToObject(root, "gnss_fix_quality_str", gnss_fix_quality_str(fix.quality));
        cJSON_AddNumberToObject(root, "gnss_satellites_used", fix.satellites_used);
        cJSON_AddNumberToObject(root, "gnss_hdop", fix.hdop);
        cJSON_AddNumberToObject(root, "gnss_altitude_m", fix.altitude_m);
        cJSON_AddNumberToObject(root, "gnss_diff_age_s", fix.diff_age_s);
        double fix_age_s = (esp_timer_get_time() - fix.last_update_us) / 1e6;
        cJSON_AddNumberToObject(root, "gnss_fix_age_s", fix_age_s);
    }

    int8_t wifi_rssi;
    if (wifi_link_get_rssi(&wifi_rssi)) {
        cJSON_AddNumberToObject(root, "wifi_rssi_dbm", wifi_rssi);
    } else {
        cJSON_AddNullToObject(root, "wifi_rssi_dbm");
    }

    // Segnale e operatore del modem, letti al massimo ogni 30 s (ogni 60 s se
    // il modem non risponde). Prima ogni richiesta di questa pagina (ogni 3 s
    // dal pannello) interrogava il modem: con il modem assente aspettava i
    // timeout e la risposta richiedeva 3,5 s, tenendo occupato il server web
    // quasi di continuo (misurato il 03/10/2026).
    static struct {
        int64_t at_us;
        bool sig_ok, op_ok;
        int rssi;
        char op[32];
        char tech[16];
    } cell;
    int64_t now_us = esp_timer_get_time();
    int64_t max_age = (cell.sig_ok || cell.op_ok) ? 30LL * 1000000 : 60LL * 1000000;
    if (cell.at_us == 0 || now_us - cell.at_us > max_age) {
        cell.at_us = now_us;
        cell.sig_ok = cellular_link_get_signal(&cell.rssi);
        cell.op_ok = cell.sig_ok &&
                     cellular_link_get_operator_info(cell.op, sizeof(cell.op), cell.tech, sizeof(cell.tech));
    }
    if (cell.sig_ok) {
        cJSON_AddNumberToObject(root, "cellular_rssi_dbm", cell.rssi);
    } else {
        cJSON_AddNullToObject(root, "cellular_rssi_dbm");
    }
    if (cell.op_ok) {
        cJSON_AddStringToObject(root, "cellular_operator", cell.op);
        cJSON_AddStringToObject(root, "cellular_tech", cell.tech);
    } else {
        cJSON_AddNullToObject(root, "cellular_operator");
        cJSON_AddNullToObject(root, "cellular_tech");
    }

    // Per il "recipiente" del collegamento NTRIP rover nella pagina
    // Segnali (vedi updateNtripFlow() in index.html): meglio qui che in
    // /api/status, dato che questa pagina la interroga gia' ogni pochi
    // secondi mentre e' visibile, a differenza della pagina Stato.
    cJSON_AddNumberToObject(root, "last_rtcm_us", (double) status_get_last_rtcm_time_us());
    cJSON_AddNumberToObject(root, "last_gga_sent_us", (double) status_get_last_gga_sent_time_us());
    cJSON_AddNumberToObject(root, "now_us", (double) esp_timer_get_time());

    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

// Copia il campo stringa solo se presente e non vuoto: una stringa vuota
// nel form significa "lascia invariato", non "cancella" - evita di
// azzerare per sbaglio una password gia' salvata quando l'utente
// aggiorna solo un altro campo.
static void copy_field(const cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

// Come copy_field, ma applica anche la stringa vuota: solo per i campi dove
// "vuoto" ha un significato preciso (es. canale di avviso spento). Ritorna
// true se il campo era presente nella richiesta.
static bool copy_field_allow_empty(const cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (!item || !cJSON_IsString(item) || !item->valuestring) {
        return false;
    }
    strncpy(dst, item->valuestring, dst_size - 1);
    dst[dst_size - 1] = '\0';
    return true;
}

// Campo numerico di pin/GPIO: -1 = non usato, valido anche come "assente"
// esplicito dal form (a differenza delle stringhe, qui il valore va
// sempre applicato se presente, incluso -1).
static void copy_pin_field(const cJSON *root, const char *key, int *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item && cJSON_IsNumber(item) && item->valueint >= -1 && item->valueint < 256) {
        *dst = item->valueint;
    }
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria esaurita");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }

    xSemaphoreTake(s_settings_edit_mutex, portMAX_DELAY);
    app_settings_t s = settings_get();

    // wifi_ssid passa da safe_utf8_to_raw_ssid_bytes() (vedi commento sopra
    // la sua definizione) invece del semplice copy_field(): puo' arrivare
    // qui da un click su un risultato della scansione WiFi, che usa la
    // stessa codifica per non perdere/alterare nomi rete con caratteri
    // speciali.
    {
        cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "wifi_ssid");
        if (ssid_item && cJSON_IsString(ssid_item) && ssid_item->valuestring[0] != '\0') {
            safe_utf8_to_raw_ssid_bytes(ssid_item->valuestring, s.wifi_ssid, sizeof(s.wifi_ssid));
        }
    }
    copy_field(root, "wifi_password", s.wifi_password, sizeof(s.wifi_password));
    copy_field(root, "cellular_apn", s.cellular_apn, sizeof(s.cellular_apn));
    // Credito della SIM (sim_tools.c): campi che si possono anche svuotare.
    {
        // Solo gli operatori del menu del pannello (prima qualunque testo,
        // troncato a 11 caratteri: collaudo del 03/10/2026).
        static const char *const ops[] = { "", "tim", "vodafone", "ho", "windtre", "postemobile", "iliad",
                                           "kena", "very", "fastweb", "coopvoce", "altro" };
        const cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "sim_operator");
        if (it && cJSON_IsString(it)) {
            for (size_t i = 0; i < sizeof(ops) / sizeof(ops[0]); i++) {
                if (strcmp(it->valuestring, ops[i]) == 0) {
                    strlcpy(s.sim_operator, ops[i], sizeof(s.sim_operator));
                    break;
                }
            }
        }
    }
    copy_field_allow_empty(root, "remote_url", s.remote_url, sizeof(s.remote_url));
    {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "vpn_enable");
        if (it && cJSON_IsBool(it)) {
            s.vpn_enable = cJSON_IsTrue(it);
        }
        it = cJSON_GetObjectItemCaseSensitive(root, "vpn_port");
        if (it && cJSON_IsNumber(it) && it->valueint > 0 && it->valueint <= 65535) {
            s.vpn_port = (uint16_t) it->valueint;
        }
    }
    copy_field_allow_empty(root, "vpn_address", s.vpn_address, sizeof(s.vpn_address));
    copy_field_allow_empty(root, "vpn_peer_public_key", s.vpn_peer_public_key, sizeof(s.vpn_peer_public_key));
    copy_field_allow_empty(root, "vpn_endpoint", s.vpn_endpoint, sizeof(s.vpn_endpoint));
    {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "remote_interval_min");
        if (it && cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 1440) {
            s.remote_interval_min = (uint16_t) it->valueint;
        }
    }
    copy_field_allow_empty(root, "sim_credit_code", s.sim_credit_code, sizeof(s.sim_credit_code));
    copy_field_allow_empty(root, "sim_credit_sms_number", s.sim_credit_sms_number, sizeof(s.sim_credit_sms_number));
    copy_field_allow_empty(root, "sim_credit_sms_text", s.sim_credit_sms_text, sizeof(s.sim_credit_sms_text));
    {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(root, "sim_credit_mode");
        if (it && cJSON_IsString(it)) {
            s.sim_credit_mode = strcmp(it->valuestring, "sms") == 0 ? 1 : 0;
        }
        it = cJSON_GetObjectItemCaseSensitive(root, "sim_credit_every_days");
        if (it && cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 365) {
            s.sim_credit_every_days = (uint16_t) it->valueint;
        }
        it = cJSON_GetObjectItemCaseSensitive(root, "sim_credit_hour");
        if (it && cJSON_IsNumber(it) && it->valueint >= 0 && it->valueint <= 23) {
            s.sim_credit_hour = (uint8_t) it->valueint;
        }
        it = cJSON_GetObjectItemCaseSensitive(root, "sim_credit_min_eur");
        if (it && cJSON_IsNumber(it) && it->valuedouble >= 0 && it->valuedouble <= 1000) {
            s.sim_credit_min_eur = (float) it->valuedouble;
        }
    }
    copy_field(root, "ntrip_host", s.ntrip_host, sizeof(s.ntrip_host));
    copy_field(root, "ntrip_mountpoint", s.ntrip_mountpoint, sizeof(s.ntrip_mountpoint));
    copy_field(root, "rover_mountpoint", s.rover_mountpoint, sizeof(s.rover_mountpoint));
    copy_field(root, "rover_username", s.rover_username, sizeof(s.rover_username));
    copy_field(root, "rover_password", s.rover_password, sizeof(s.rover_password));
    copy_field(root, "ntrip_password", s.ntrip_password, sizeof(s.ntrip_password));
    copy_field(root, "ap_ssid", s.ap_ssid, sizeof(s.ap_ssid));
    copy_field(root, "ap_password", s.ap_password, sizeof(s.ap_password));
    copy_field(root, "admin_code", s.admin_code, sizeof(s.admin_code));
    copy_field(root, "device_serial", s.device_serial, sizeof(s.device_serial));
    copy_field(root, "ota_update_url", s.ota_update_url, sizeof(s.ota_update_url));
    // Host SMTP, destinatario email e numero WhatsApp: vuoto = canale spento
    // (vedi alerts.c), quindi si puo' salvare anche vuoto.
    copy_field_allow_empty(root, "alert_smtp_host", s.alert_smtp_host, sizeof(s.alert_smtp_host));
    copy_field(root, "alert_smtp_user", s.alert_smtp_user, sizeof(s.alert_smtp_user));
    copy_field(root, "alert_smtp_password", s.alert_smtp_password, sizeof(s.alert_smtp_password));
    copy_field_allow_empty(root, "alert_email_to", s.alert_email_to, sizeof(s.alert_email_to));
    copy_field_allow_empty(root, "alert_whatsapp_phone", s.alert_whatsapp_phone, sizeof(s.alert_whatsapp_phone));
    copy_field(root, "alert_whatsapp_apikey", s.alert_whatsapp_apikey, sizeof(s.alert_whatsapp_apikey));
    copy_field(root, "ntrip_caster_server_mountpoint", s.ntrip_caster_server_mountpoint, sizeof(s.ntrip_caster_server_mountpoint));
    copy_field(root, "ntrip_caster_server_password", s.ntrip_caster_server_password, sizeof(s.ntrip_caster_server_password));
    // Utente del caster locale vuoto = rover accettati senza credenziali. Il
    // caster locale le chiede se utente O password non sono vuoti, e la
    // password vuota nel form vuol dire "non modificare": senza cancellarla
    // qui non ci sarebbe modo di togliere l'autenticazione.
    if (copy_field_allow_empty(root, "ntrip_caster_server_username", s.ntrip_caster_server_username,
                               sizeof(s.ntrip_caster_server_username))
        && s.ntrip_caster_server_username[0] == '\0') {
        s.ntrip_caster_server_password[0] = '\0';
    }

    cJSON *alert_enable_item = cJSON_GetObjectItemCaseSensitive(root, "alert_enable");
    if (alert_enable_item && cJSON_IsBool(alert_enable_item)) {
        s.alert_enable = cJSON_IsTrue(alert_enable_item);
    }
    cJSON *data_plan_item = cJSON_GetObjectItemCaseSensitive(root, "data_plan_mb");
    if (data_plan_item && cJSON_IsNumber(data_plan_item) && data_plan_item->valuedouble >= 0 &&
        data_plan_item->valuedouble <= 1000000) {
        s.data_plan_mb = (uint32_t) data_plan_item->valuedouble;
    }
    // Rinnovo del piano della SIM: data "aaaa-mm-gg" (vuota = nessuna).
    cJSON *renew_date_item = cJSON_GetObjectItemCaseSensitive(root, "sim_renew_date");
    if (renew_date_item && cJSON_IsString(renew_date_item)) {
        unsigned y = 0, m = 0, d = 0;
        if (renew_date_item->valuestring[0] == 0) {
            s.sim_renew_date = 0;
        } else if (sscanf(renew_date_item->valuestring, "%u-%u-%u", &y, &m, &d) == 3 &&
                   y >= 2020 && y <= 2100 && m >= 1 && m <= 12 && d >= 1 && d <= 31) {
            s.sim_renew_date = y * 10000 + m * 100 + d;
        }
    }
    cJSON *renew_mode_item = cJSON_GetObjectItemCaseSensitive(root, "sim_renew_mode");
    if (renew_mode_item && cJSON_IsString(renew_mode_item)) {
        const char *m = renew_mode_item->valuestring;
        s.sim_renew_mode = strcmp(m, "monthly") == 0 ? SIM_RENEW_MONTHLY
                         : (strcmp(m, "days") == 0 ? SIM_RENEW_DAYS : SIM_RENEW_SINGLE);
    }
    struct { const char *name; uint16_t *field; int min, max; } renew_nums[] = {
        { "sim_renew_every_days", &s.sim_renew_every_days, 1, 366 },
        { "sim_notice1_days", &s.sim_notice1_days, 0, 60 },
        { "sim_notice2_days", &s.sim_notice2_days, 0, 60 },
    };
    for (size_t i = 0; i < sizeof(renew_nums) / sizeof(renew_nums[0]); i++) {
        cJSON *it = cJSON_GetObjectItemCaseSensitive(root, renew_nums[i].name);
        if (it && cJSON_IsNumber(it) && it->valueint >= renew_nums[i].min && it->valueint <= renew_nums[i].max) {
            *renew_nums[i].field = (uint16_t) it->valueint;
        }
    }

    // Il pannello lo chiede in GB (con decimali): salvato in MB come prima.
    cJSON *data_plan_gb_item = cJSON_GetObjectItemCaseSensitive(root, "data_plan_gb");
    if (data_plan_gb_item && cJSON_IsNumber(data_plan_gb_item) && data_plan_gb_item->valuedouble >= 0 &&
        data_plan_gb_item->valuedouble <= 1000) {
        s.data_plan_mb = (uint32_t) (data_plan_gb_item->valuedouble * 1000 + 0.5);
    }
    cJSON *alert_threshold_item = cJSON_GetObjectItemCaseSensitive(root, "alert_threshold_min");
    if (alert_threshold_item && cJSON_IsNumber(alert_threshold_item) && alert_threshold_item->valueint > 0) {
        s.alert_threshold_min = (uint16_t) alert_threshold_item->valueint;
    }
    cJSON *alert_smtp_port_item = cJSON_GetObjectItemCaseSensitive(root, "alert_smtp_port");
    if (alert_smtp_port_item && cJSON_IsNumber(alert_smtp_port_item) &&
        alert_smtp_port_item->valueint > 0 && alert_smtp_port_item->valueint <= 65535) {
        s.alert_smtp_port = (uint16_t) alert_smtp_port_item->valueint;
    }
    cJSON *drift_enable_item = cJSON_GetObjectItemCaseSensitive(root, "base_drift_alert_enable");
    if (drift_enable_item && cJSON_IsBool(drift_enable_item)) {
        s.base_drift_alert_enable = cJSON_IsTrue(drift_enable_item);
    }
    cJSON *drift_threshold_item = cJSON_GetObjectItemCaseSensitive(root, "base_drift_threshold_m");
    if (drift_threshold_item && cJSON_IsNumber(drift_threshold_item) && drift_threshold_item->valuedouble > 0) {
        s.base_drift_threshold_m = (float) drift_threshold_item->valuedouble;
    }
    cJSON *caster_srv_enable_item = cJSON_GetObjectItemCaseSensitive(root, "ntrip_caster_server_enable");
    if (caster_srv_enable_item && cJSON_IsBool(caster_srv_enable_item)) {
        s.ntrip_caster_server_enable = cJSON_IsTrue(caster_srv_enable_item);
    }
    cJSON *caster_srv_port_item = cJSON_GetObjectItemCaseSensitive(root, "ntrip_caster_server_port");
    if (caster_srv_port_item && cJSON_IsNumber(caster_srv_port_item) &&
        caster_srv_port_item->valueint > 0 && caster_srv_port_item->valueint <= 65535) {
        s.ntrip_caster_server_port = (uint16_t) caster_srv_port_item->valueint;
    }

    cJSON *base_pos_mode_item = cJSON_GetObjectItemCaseSensitive(root, "base_position_mode");
    if (base_pos_mode_item && cJSON_IsString(base_pos_mode_item)) {
        s.base_position_mode = (strcmp(base_pos_mode_item->valuestring, "manual") == 0)
                                    ? BASE_POSITION_MANUAL : BASE_POSITION_AUTO;
    }
    cJSON *base_lat_item = cJSON_GetObjectItemCaseSensitive(root, "base_fixed_lat_deg");
    if (base_lat_item && cJSON_IsNumber(base_lat_item)) {
        s.base_fixed_lat_deg = base_lat_item->valuedouble;
    }
    cJSON *base_lon_item = cJSON_GetObjectItemCaseSensitive(root, "base_fixed_lon_deg");
    if (base_lon_item && cJSON_IsNumber(base_lon_item)) {
        s.base_fixed_lon_deg = base_lon_item->valuedouble;
    }
    cJSON *base_height_item = cJSON_GetObjectItemCaseSensitive(root, "base_fixed_height_m");
    if (base_height_item && cJSON_IsNumber(base_height_item)) {
        s.base_fixed_height_m = base_height_item->valuedouble;
    }
    // Survey-in: 0 = predefinito del chip. Durata fino a 65535 s (~18 h),
    // precisione tra 1 cm e 100 m.
    cJSON *svin_dur_item = cJSON_GetObjectItemCaseSensitive(root, "base_svin_min_dur_s");
    if (svin_dur_item && cJSON_IsNumber(svin_dur_item) && svin_dur_item->valuedouble >= 0 &&
        svin_dur_item->valuedouble <= 65535) {
        s.base_svin_min_dur_s = (uint16_t) svin_dur_item->valuedouble;
    }
    cJSON *svin_acc_item = cJSON_GetObjectItemCaseSensitive(root, "base_svin_acc_m");
    if (svin_acc_item && cJSON_IsNumber(svin_acc_item) &&
        (svin_acc_item->valuedouble == 0 || (svin_acc_item->valuedouble >= 0.01 && svin_acc_item->valuedouble <= 100))) {
        s.base_svin_acc_m = (float) svin_acc_item->valuedouble;
    }

    cJSON *auto_update_enable_item = cJSON_GetObjectItemCaseSensitive(root, "auto_update_check_enable");
    if (auto_update_enable_item && cJSON_IsBool(auto_update_enable_item)) {
        s.auto_update_check_enable = cJSON_IsTrue(auto_update_enable_item);
    }
    cJSON *auto_update_interval_item = cJSON_GetObjectItemCaseSensitive(root, "auto_update_check_interval_h");
    if (auto_update_interval_item && cJSON_IsNumber(auto_update_interval_item) && auto_update_interval_item->valueint > 0) {
        s.auto_update_check_interval_h = (uint16_t) auto_update_interval_item->valueint;
    }

    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "ntrip_port");
    if (port_item && cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535) {
        s.ntrip_port = (uint16_t) port_item->valueint;
    }

    cJSON *udp_port_item = cJSON_GetObjectItemCaseSensitive(root, "nmea_udp_port");
    if (udp_port_item && cJSON_IsNumber(udp_port_item) && udp_port_item->valueint > 0 && udp_port_item->valueint <= 65535) {
        s.nmea_udp_port = (uint16_t) udp_port_item->valueint;
    }

    cJSON *chip_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_chip");
    if (chip_item && cJSON_IsString(chip_item)) {
        if (strcmp(chip_item->valuestring, "unicore") == 0) {
            s.gnss_chip = GNSS_CHIP_UNICORE;
        } else if (strcmp(chip_item->valuestring, "lc29h") == 0) {
            s.gnss_chip = GNSS_CHIP_LC29H;
        } else if (strcmp(chip_item->valuestring, "bynav") == 0) {
            s.gnss_chip = GNSS_CHIP_BYNAV;
        } else if (strcmp(chip_item->valuestring, "bynav_m21d") == 0) {
            s.gnss_chip = GNSS_CHIP_BYNAV_M21D;
        } else if (strcmp(chip_item->valuestring, "l76k") == 0) {
            s.gnss_chip = GNSS_CHIP_L76K;
        } else {
            s.gnss_chip = GNSS_CHIP_UBLOX;
        }
    }

    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "device_mode");
    if (mode_item && cJSON_IsString(mode_item)) {
        s.device_mode = (strcmp(mode_item->valuestring, "rover") == 0) ? DEVICE_MODE_ROVER : DEVICE_MODE_BASE;
    }

    {
        // Braccio di leva antenne + orientamento RBV (solo GNSS_CHIP_
        // BYNAV_M21D rover, vedi gnss_bynav_m21d_configure_rover()) - stesso
        // pattern dei campi RTCM booleani sopra, qui puntatori a float.
        struct { const char *json_key; float *field; } bynav_float_fields[] = {
            { "bynav_ant1_x_m", &s.bynav_ant1_x_m },
            { "bynav_ant1_y_m", &s.bynav_ant1_y_m },
            { "bynav_ant1_z_m", &s.bynav_ant1_z_m },
            { "bynav_ant2_x_m", &s.bynav_ant2_x_m },
            { "bynav_ant2_y_m", &s.bynav_ant2_y_m },
            { "bynav_ant2_z_m", &s.bynav_ant2_z_m },
            { "bynav_rbv_roll_deg", &s.bynav_rbv_roll_deg },
            { "bynav_rbv_pitch_deg", &s.bynav_rbv_pitch_deg },
            { "bynav_rbv_yaw_deg", &s.bynav_rbv_yaw_deg },
        };
        for (size_t i = 0; i < sizeof(bynav_float_fields) / sizeof(bynav_float_fields[0]); i++) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(root, bynav_float_fields[i].json_key);
            if (item && cJSON_IsNumber(item)) {
                *bynav_float_fields[i].field = (float) item->valuedouble;
            }
        }
    }

    {
        // Un checkbox indipendente per numero messaggio RTCM3 (v1.19.42) -
        // stesso pattern ripetuto per ciascuno, nessuna interdipendenza tra
        // di loro (vedi commento su app_settings_t.rtcm_1007_enable ecc. in
        // settings.h). Puntatori dentro "s" (variabile locale), non
        // puntatori a membro (questo e' C, non C++).
        struct { const char *json_key; bool *field; } rtcm_bool_fields[] = {
            { "rtcm_1005_enable", &s.rtcm_1005_enable },
            { "rtcm_1230_enable", &s.rtcm_1230_enable },
            { "rtcm_1007_enable", &s.rtcm_1007_enable },
            { "rtcm_1008_enable", &s.rtcm_1008_enable },
            { "rtcm_1019_enable", &s.rtcm_1019_enable },
            { "rtcm_1020_enable", &s.rtcm_1020_enable },
            { "rtcm_1074_enable", &s.rtcm_1074_enable },
            { "rtcm_1077_enable", &s.rtcm_1077_enable },
            { "rtcm_1084_enable", &s.rtcm_1084_enable },
            { "rtcm_1087_enable", &s.rtcm_1087_enable },
            { "rtcm_1094_enable", &s.rtcm_1094_enable },
            { "rtcm_1097_enable", &s.rtcm_1097_enable },
            { "rtcm_1124_enable", &s.rtcm_1124_enable },
            { "rtcm_1127_enable", &s.rtcm_1127_enable },
        };
        for (size_t i = 0; i < sizeof(rtcm_bool_fields) / sizeof(rtcm_bool_fields[0]); i++) {
            cJSON *item = cJSON_GetObjectItemCaseSensitive(root, rtcm_bool_fields[i].json_key);
            if (item && cJSON_IsBool(item)) {
                *rtcm_bool_fields[i].field = cJSON_IsTrue(item);
            }
        }
    }

    cJSON *cellular_modem_item = cJSON_GetObjectItemCaseSensitive(root, "cellular_is_sim868");
    if (cellular_modem_item && cJSON_IsBool(cellular_modem_item)) {
        s.cellular_is_sim868 = cJSON_IsTrue(cellular_modem_item);
    }

    cJSON *module_item = cJSON_GetObjectItemCaseSensitive(root, "cellular_module");
    if (module_item && cJSON_IsString(module_item)) {
        const char *m = module_item->valuestring;
        if (strcmp(m, "sim868") == 0) {
            s.cellular_is_sim868 = true;
        } else if (strcmp(m, "sim7600_simcom") == 0) {
            s.cellular_is_sim868 = false;
            s.cellular_simcom_std = true;
        } else if (strcmp(m, "sim7600_lilygo") == 0) {
            s.cellular_is_sim868 = false;
            s.cellular_simcom_std = false;
        }
    }

    cJSON *net_mode_item = cJSON_GetObjectItemCaseSensitive(root, "network_mode");
    if (net_mode_item && cJSON_IsString(net_mode_item)) {
        if (strcmp(net_mode_item->valuestring, "wifi") == 0) {
            s.network_mode = NETWORK_MODE_WIFI_ONLY;
        } else if (strcmp(net_mode_item->valuestring, "cellular") == 0) {
            s.network_mode = NETWORK_MODE_CELLULAR_ONLY;
        } else if (strcmp(net_mode_item->valuestring, "ethernet_only") == 0) {
            s.network_mode = NETWORK_MODE_ETHERNET_ONLY;
        } else {
            s.network_mode = NETWORK_MODE_BOTH;
        }
    }

    cJSON *gnss_uart_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_uart_num");
    if (gnss_uart_item && cJSON_IsNumber(gnss_uart_item) && gnss_uart_item->valueint >= 0 && gnss_uart_item->valueint < 3) {
        s.gnss_uart_num = gnss_uart_item->valueint;
    }
    copy_pin_field(root, "gnss_uart_tx_pin", &s.gnss_uart_tx_pin);
    copy_pin_field(root, "gnss_uart_rx_pin", &s.gnss_uart_rx_pin);
    cJSON *gnss_baud_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_uart_baud");
    if (gnss_baud_item && cJSON_IsNumber(gnss_baud_item) && gnss_baud_item->valueint > 0) {
        s.gnss_uart_baud = gnss_baud_item->valueint;
    }
    cJSON *gnss_link_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_link");
    if (gnss_link_item && cJSON_IsString(gnss_link_item)) {
        s.gnss_i2c = (strcmp(gnss_link_item->valuestring, "i2c") == 0);
    }

    cJSON *rgb_mode_item = cJSON_GetObjectItemCaseSensitive(root, "rgb_led_mode");
    if (rgb_mode_item && cJSON_IsString(rgb_mode_item)) {
        if (strcmp(rgb_mode_item->valuestring, "ws2812") == 0) {
            s.rgb_led_mode = RGB_LED_WS2812;
        } else if (strcmp(rgb_mode_item->valuestring, "pwm3") == 0) {
            s.rgb_led_mode = RGB_LED_PWM3;
        } else {
            s.rgb_led_mode = RGB_LED_NONE;
        }
    }
    copy_pin_field(root, "rgb_led_ws2812_pin", &s.rgb_led_ws2812_pin);
    copy_pin_field(root, "rgb_led_pwm_r_pin", &s.rgb_led_pwm_r_pin);
    copy_pin_field(root, "rgb_led_pwm_g_pin", &s.rgb_led_pwm_g_pin);
    copy_pin_field(root, "rgb_led_pwm_b_pin", &s.rgb_led_pwm_b_pin);
    cJSON *rgb_active_low_item = cJSON_GetObjectItemCaseSensitive(root, "rgb_led_pwm_active_low");
    if (rgb_active_low_item && cJSON_IsBool(rgb_active_low_item)) {
        s.rgb_led_pwm_active_low = cJSON_IsTrue(rgb_active_low_item);
    }

    copy_pin_field(root, "oled_sda_pin", &s.oled_sda_pin);
    copy_pin_field(root, "oled_scl_pin", &s.oled_scl_pin);
    cJSON *oled_addr_item = cJSON_GetObjectItemCaseSensitive(root, "oled_i2c_addr");
    if (oled_addr_item && cJSON_IsNumber(oled_addr_item) && oled_addr_item->valueint > 0 && oled_addr_item->valueint < 256) {
        s.oled_i2c_addr = (uint8_t) oled_addr_item->valueint;
    }
    cJSON *oled_controller_item = cJSON_GetObjectItemCaseSensitive(root, "oled_controller");
    if (oled_controller_item && cJSON_IsString(oled_controller_item)) {
        if (strcmp(oled_controller_item->valuestring, "sh1106") == 0) {
            s.oled_controller = OLED_CTRL_SH1106;
        } else if (strcmp(oled_controller_item->valuestring, "ssd1309") == 0) {
            s.oled_controller = OLED_CTRL_SSD1309;
        } else {
            s.oled_controller = OLED_CTRL_SSD1306;
        }
    }
    cJSON *oled_flip_h_item = cJSON_GetObjectItemCaseSensitive(root, "oled_flip_h");
    if (oled_flip_h_item && cJSON_IsBool(oled_flip_h_item)) {
        s.oled_flip_h = cJSON_IsTrue(oled_flip_h_item);
    }
    cJSON *oled_flip_v_item = cJSON_GetObjectItemCaseSensitive(root, "oled_flip_v");
    if (oled_flip_v_item && cJSON_IsBool(oled_flip_v_item)) {
        s.oled_flip_v = cJSON_IsTrue(oled_flip_v_item);
    }

    cJSON_Delete(root);

    esp_err_t err = settings_save(&s);
    xSemaphoreGive(s_settings_edit_mutex);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "salvataggio fallito");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Contesto per ota_read_from_http(): legge dal corpo della richiesta HTTP
// finche' non sono stati consumati tutti i content_len byte annunciati.
typedef struct {
    httpd_req_t *req;
    int remaining;
} http_ota_ctx_t;

static int ota_read_from_http(void *ctx_ptr, uint8_t *buf, size_t max_len)
{
    http_ota_ctx_t *ctx = (http_ota_ctx_t *) ctx_ptr;
    if (ctx->remaining <= 0) {
        return 0;
    }
    size_t to_read = max_len < (size_t) ctx->remaining ? max_len : (size_t) ctx->remaining;
    int r = httpd_req_recv(ctx->req, (char *) buf, to_read);
    if (r <= 0) {
        return -1;
    }
    ctx->remaining -= r;
    return r;
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo mancante");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Aggiornamento firmware ricevuto dal browser (%d byte)...", req->content_len);

    http_ota_ctx_t ctx = { .req = req, .remaining = req->content_len };
    esp_err_t err = ota_update_apply(ota_read_from_http, &ctx);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "aggiornamento fallito, immagine non applicata");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Firmware aggiornato, riavvio in corso");
    sys_stats_note_restart_reason("aggiornamento firmware dal pannello");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_sd_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char msg[96] = {0};
    bool applied = sd_update_check_and_apply(msg, sizeof(msg));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddStringToObject(root, "message", msg);
    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);

    if (applied) {
        ESP_LOGI(TAG, "Firmware aggiornato da microSD, riavvio in corso");
        sys_stats_note_restart_reason("aggiornamento firmware da microSD");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return ESP_OK;
}

static esp_err_t fw_archive_list_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    fw_archive_entry_t entries[16];
    size_t n = fw_archive_list(entries, sizeof(entries) / sizeof(entries[0]));

    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON_AddItemToArray(arr, cJSON_CreateString(entries[i].filename));
    }
    cJSON_AddItemToObject(root, "files", arr);
    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t fw_archive_apply_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }
    char buf[257];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    cJSON *filename_item = root ? cJSON_GetObjectItemCaseSensitive(root, "filename") : NULL;
    if (!filename_item || !cJSON_IsString(filename_item)) {
        if (root) cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nome file mancante");
        return ESP_FAIL;
    }
    char filename[32];
    strncpy(filename, filename_item->valuestring, sizeof(filename) - 1);
    filename[sizeof(filename) - 1] = '\0';
    cJSON_Delete(root);

    char msg[96] = {0};
    bool applied = fw_archive_apply(filename, msg, sizeof(msg));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "applied", applied);
    cJSON_AddStringToObject(resp, "message", msg);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);

    if (applied) {
        ESP_LOGW(TAG, "Firmware ripristinato manualmente da archivio SD, riavvio in corso");
        sys_stats_note_restart_reason("ripristino firmware dall'archivio SD");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return ESP_OK;
}

// Stato del test di connessione WiFi in corso, letto dalla UI web mentre
// il tentativo gira in background - vedi wifi_test_connect_task() sotto
// per il motivo per cui non gira piu' direttamente nel task del server web.
typedef struct {
    bool running;
    bool done;
    bool connected; // valido solo se done == true
} wifi_test_status_t;

static SemaphoreHandle_t s_wifi_test_mutex;
static wifi_test_status_t s_wifi_test_status;

bool web_ui_wifi_test_in_progress(void)
{
    if (!s_wifi_test_mutex) {
        return false;
    }
    xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
    bool running = s_wifi_test_status.running;
    xSemaphoreGive(s_wifi_test_mutex);
    return running;
}

typedef struct {
    char ssid[33];
    char password[65];
} wifi_test_task_ctx_t;

static void wifi_test_connect_task(void *arg)
{
    wifi_test_task_ctx_t *ctx = (wifi_test_task_ctx_t *) arg;

    bool connected = wifi_link_connect_with(ctx->ssid, ctx->password, 15000);
    if (connected) {
        xSemaphoreTake(s_settings_edit_mutex, portMAX_DELAY);
        app_settings_t s = settings_get();
        // Ricorda questa rete (diventa la principale; un'eventuale rete
        // precedente scende nell'elenco delle "conosciute") invece di
        // limitarsi a sovrascrivere wifi_ssid/password - vedi
        // settings.h, permette di spostare il dispositivo tra piu' reti
        // gia' provate con successo senza dover reinserire le credenziali.
        app_settings_remember_wifi(&s, ctx->ssid, ctx->password);
        settings_save(&s);
        xSemaphoreGive(s_settings_edit_mutex);
        status_set_net(NET_STATUS_WIFI);
        ESP_LOGI(TAG, "Connesso, rete '%s' ricordata automaticamente", ctx->ssid);
    }

    xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
    s_wifi_test_status.running = false;
    s_wifi_test_status.done = true;
    s_wifi_test_status.connected = connected;
    xSemaphoreGive(s_wifi_test_mutex);

    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t wifi_test_connect_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char buf[257];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }
    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!ssid_item || !cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID mancante");
        return ESP_FAIL;
    }

    char ssid[33];
    safe_utf8_to_raw_ssid_bytes(ssid_item->valuestring, ssid, sizeof(ssid));

    // Password vuota/assente = mantieni quella gia' salvata per questo
    // SSID (stessa convenzione dei campi password nel resto della UI) -
    // utile per ritestare una rete gia' configurata senza riscriverla.
    char password[65];
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (pass_item && cJSON_IsString(pass_item) && strlen(pass_item->valuestring) > 0) {
        strncpy(password, pass_item->valuestring, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';
    } else {
        // Password vuota = usa quella gia' salvata per QUESTO ssid - prima
        // cercava solo in wifi_password (quella della rete "principale"),
        // sbagliato se l'utente sceglie dall'elenco scansione una rete gia'
        // nota ma diversa dalla principale attuale: si finiva per riprovare
        // con la password della rete SBAGLIATA invece di quella salvata per
        // la rete effettivamente selezionata. Bug reale segnalato
        // dall'utente ("quando ritorno su una rete che gia' conosce non
        // devo mettere la password"). Cerca prima tra le principale, poi
        // tra le reti "conosciute" (vedi settings.h).
        app_settings_t existing = settings_get();
        password[0] = '\0';
        if (strcmp(existing.wifi_ssid, ssid) == 0) {
            strncpy(password, existing.wifi_password, sizeof(password) - 1);
        } else {
            for (int i = 0; i < WIFI_KNOWN_NETWORKS_MAX; i++) {
                if (strcmp(existing.wifi_known_networks[i].ssid, ssid) == 0) {
                    strncpy(password, existing.wifi_known_networks[i].password, sizeof(password) - 1);
                    break;
                }
            }
        }
        password[sizeof(password) - 1] = '\0';
    }
    cJSON_Delete(root);

    // Avviato in un task separato invece di bloccare qui fino a 15s: il
    // tentativo obbliga la radio a spostarsi sul canale della rete di
    // destinazione per autenticarsi, il che puo' disturbare momentaneamente
    // il collegamento della pagina stessa (sempre sull'AP di setup, canale
    // 1) - confermato su hardware reale ("spesso si blocca" durante il
    // test). Se il server web resta bloccato proprio in quel momento, la
    // richiesta HTTP puo' restare sospesa indefinitamente lato browser.
    if (!s_wifi_test_mutex) {
        s_wifi_test_mutex = xSemaphoreCreateMutex();
    }
    // Un secondo tentativo partito mentre il primo e' ancora in corso (es.
    // doppio click su "Connetti") avviava DUE task concorrenti, entrambi
    // basati sulla stessa istantanea di partenza di s_settings: ciascuno
    // decide da solo come spostare la rete "principale" precedente
    // nell'elenco delle "conosciute" (vedi app_settings_remember_wifi()),
    // senza sapere dell'altro - chi salva per ultimo sovrascrive il lavoro
    // dell'altro, un classico read-modify-write non atomico nonostante
    // ogni singola lettura/scrittura di s_settings sia gia' protetta da
    // mutex. Rifiutare qui un secondo tentativo mentre uno e' gia' attivo
    // lo rende impossibile strutturalmente, invece di sperare che l'utente
    // non prema due volte.
    xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
    bool already_running = s_wifi_test_status.running;
    if (!already_running) {
        s_wifi_test_status = (wifi_test_status_t){ .running = true };
    }
    xSemaphoreGive(s_wifi_test_mutex);
    if (already_running) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "Un test di connessione e' gia' in corso, attendi che finisca");
        return ESP_FAIL;
    }

    wifi_test_task_ctx_t *ctx = calloc(1, sizeof(wifi_test_task_ctx_t));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria esaurita");
        return ESP_FAIL;
    }
    strncpy(ctx->ssid, ssid, sizeof(ctx->ssid) - 1);
    strncpy(ctx->password, password, sizeof(ctx->password) - 1);

    ESP_LOGI(TAG, "Test connessione WiFi a '%s' avviato in background", ssid);
    // 4096 non bastava piu': causava uno stack overflow reale (confermato
    // su hardware, log seriale) proprio nel momento in cui una connessione
    // riusciva e la funzione chiamava settings_save() subito dopo - il
    // crash interrompeva il salvataggio prima di nvs_commit(), quindi le
    // credenziali digitate non venivano MAI persistite: la causa vera
    // dietro "il dispositivo non ricorda piu' la password del WiFi".
    // Stessa causa/fix gia' visto altrove in questo progetto (stack
    // troppo piccolo per un task che finisce per fare I/O NVS + log).
    if (xTaskCreate(wifi_test_connect_task, "wifi_test", 8192, ctx, 5, NULL) != pdPASS) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "avvio task fallito");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_test_progress_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_test_status_t st = {0};
    if (s_wifi_test_mutex) {
        xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
        st = s_wifi_test_status;
        xSemaphoreGive(s_wifi_test_mutex);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "running", st.running);
    cJSON_AddBoolToObject(resp, "done", st.done);
    cJSON_AddBoolToObject(resp, "connected", st.connected);
    if (st.done) {
        // Nome REALE della rete a cui ci si e' effettivamente associati ORA
        // (dal driver, non dal campo SSID digitato) - mostrato SEMPRE a
        // test finito, anche quando il test e' fallito (st.connected ==
        // false): un test fallito lascia comunque partire il riconnettore
        // automatico, che puo' ricollegarsi da solo a una rete diversa gia'
        // nota (es. quella su cui si era prima del test) - senza questo
        // campo il messaggio "NON connesso" lasciava intendere che il
        // dispositivo fosse rimasto scollegato del tutto, mentre magari
        // era gia' di nuovo online su un'altra rete, causando confusione
        // reale segnalata dall'utente.
        char current_ssid[33];
        if (wifi_link_get_current_ssid(current_ssid, sizeof(current_ssid))) {
            char safe_ssid[65];
            raw_ssid_bytes_to_safe_utf8(current_ssid, safe_ssid, sizeof(safe_ssid));
            cJSON_AddStringToObject(resp, "connected_ssid", safe_ssid);
        }
    }
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_scan_result_t results[16];
    size_t n = wifi_link_scan(results, 16);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *net = cJSON_CreateObject();
        char safe_ssid[65]; // fino a 2 byte UTF-8 per ogni byte originale (max 32)
        raw_ssid_bytes_to_safe_utf8(results[i].ssid, safe_ssid, sizeof(safe_ssid));
        cJSON_AddStringToObject(net, "ssid", safe_ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(net, "secure", results[i].secure);
        cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddItemToObject(root, "networks", networks);

    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t ntrip_mountpoints_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    ntrip_mountpoint_entry_t entries[NTRIP_MOUNTPOINTS_MAX];
    size_t n = ntrip_rover_client_fetch_mountpoints(entries, NTRIP_MOUNTPOINTS_MAX);

    cJSON *root = cJSON_CreateObject();
    cJSON *mountpoints = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *mp = cJSON_CreateObject();
        cJSON_AddStringToObject(mp, "name", entries[i].name);
        cJSON_AddStringToObject(mp, "description", entries[i].description);
        cJSON_AddItemToArray(mountpoints, mp);
    }
    cJSON_AddItemToObject(root, "mountpoints", mountpoints);

    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

typedef struct {
    httpd_req_t *req;   // copia asincrona della richiesta HTTP
    bool as_base;
    char host[65];
    uint16_t port;
    char mountpoint[33];
    char username[33];
    char password[65];
} ntrip_test_job_t;

static void ntrip_test_task(void *arg)
{
    ntrip_test_job_t *job = arg;
    char msg[200];
    bool ok = job->as_base
        ? ntrip_client_test_source(job->host, job->port, job->mountpoint, job->password, msg, sizeof(msg))
        : ntrip_rover_client_test_connect(job->host, job->port, job->mountpoint, job->username,
                                          job->password, msg, sizeof(msg));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    cJSON_AddStringToObject(resp, "message", msg);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(job->req, "application/json");
    httpd_resp_set_hdr(job->req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(job->req, json);
    free(json);
    cJSON_Delete(resp);

    httpd_req_async_handler_complete(job->req);
    memset(job->password, 0, sizeof(job->password));
    free(job);
    vTaskDelete(NULL);
}

// Prova subito una connessione NTRIP con i parametri inviati dalla pagina
// (non ancora necessariamente salvati) - stessa idea del "Connetti" per il
// WiFi. La prova vera gira in ntrip_test_task, vedi in fondo alla funzione.
static esp_err_t ntrip_test_connect_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char buf[513];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }

    app_settings_t existing = settings_get();

    char host[65];
    cJSON *host_item = cJSON_GetObjectItemCaseSensitive(root, "host");
    if (host_item && cJSON_IsString(host_item) && host_item->valuestring[0] != '\0') {
        strncpy(host, host_item->valuestring, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    } else {
        strncpy(host, existing.ntrip_host, sizeof(host) - 1);
        host[sizeof(host) - 1] = '\0';
    }

    uint16_t port = existing.ntrip_port;
    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "port");
    if (port_item && cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535) {
        port = (uint16_t) port_item->valueint;
    }

    // Modalita' da provare: quella scelta nel pannello (anche se non ancora
    // salvata), altrimenti quella salvata. In base si prova come sorgente
    // (SOURCE + password sorgente, come fa davvero la base), in rover come
    // ricevitore. I campi non inviati si prendono dalle credenziali salvate
    // della stessa modalita' (base: ntrip_*, rover: rover_*, dalla 1.19.66).
    bool as_base = (existing.device_mode == DEVICE_MODE_BASE);
    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "device_mode");
    if (mode_item && cJSON_IsString(mode_item)) {
        as_base = (strcmp(mode_item->valuestring, "rover") != 0);
    }

    char mountpoint[33];
    cJSON *mp_item = cJSON_GetObjectItemCaseSensitive(root, "mountpoint");
    const char *mp_src = (mp_item && cJSON_IsString(mp_item) && mp_item->valuestring[0] != '\0')
        ? mp_item->valuestring
        : (as_base ? existing.ntrip_mountpoint : existing.rover_mountpoint);
    strncpy(mountpoint, mp_src, sizeof(mountpoint) - 1);
    mountpoint[sizeof(mountpoint) - 1] = '\0';

    char username[33];
    cJSON *user_item = cJSON_GetObjectItemCaseSensitive(root, "username");
    const char *user_src = (user_item && cJSON_IsString(user_item) && user_item->valuestring[0] != '\0')
        ? user_item->valuestring
        : existing.rover_username;
    strncpy(username, user_src, sizeof(username) - 1);
    username[sizeof(username) - 1] = '\0';

    // Password vuota = usa quella gia' salvata (stessa convenzione del
    // resto della UI: il campo password non torna mai indietro in lettura).
    char password[65];
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    const char *pass_src = (pass_item && cJSON_IsString(pass_item) && pass_item->valuestring[0] != '\0')
        ? pass_item->valuestring
        : (as_base ? existing.ntrip_password : existing.rover_password);
    strncpy(password, pass_src, sizeof(password) - 1);
    password[sizeof(password) - 1] = '\0';
    cJSON_Delete(root);

    // La prova (DNS + connessione + risposta del caster, fino a ~15 s con un
    // caster che non risponde) gira in un task a parte con la richiesta resa
    // asincrona: fatta qui teneva fermo l'intero server web e il pannello
    // non rispondeva a nessuno per tutto quel tempo (misurato: 18 s).
    ntrip_test_job_t *job = calloc(1, sizeof(*job));
    if (!job) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
        return ESP_FAIL;
    }
    job->as_base = as_base;
    job->port = port;
    memcpy(job->host, host, sizeof(job->host));
    memcpy(job->mountpoint, mountpoint, sizeof(job->mountpoint));
    memcpy(job->username, username, sizeof(job->username));
    memcpy(job->password, password, sizeof(job->password));
    if (httpd_req_async_handler_begin(req, &job->req) != ESP_OK) {
        free(job);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "richiesta asincrona non disponibile");
        return ESP_FAIL;
    }
    if (xTaskCreate(ntrip_test_task, "ntrip_test", 6144, job, 5, NULL) != pdPASS) {
        httpd_resp_send_err(job->req, HTTPD_500_INTERNAL_SERVER_ERROR, "task di prova non avviato");
        httpd_req_async_handler_complete(job->req);
        free(job);
        return ESP_FAIL;
    }
    return ESP_OK;
}

// Formattazione della microSD in FAT32 dal pannello. Cancella tutto, quindi
// la richiesta deve contenere {"confirm":"FORMATTA"} (la pagina la manda solo
// dopo la conferma esplicita dell'utente). Su schede grandi dura anche
// minuti: gira in un task a parte con la richiesta HTTP asincrona, come la
// prova di connessione al caster, per non bloccare il pannello.
static void sd_format_task(void *arg)
{
    httpd_req_t *req = arg;
    char msg[128];
    bool ok = sd_format_card(msg, sizeof(msg));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    cJSON_AddStringToObject(resp, "message", msg);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    httpd_req_async_handler_complete(req);
    vTaskDelete(NULL);
}

static esp_err_t sd_format_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[64] = {0};
    int len = req->content_len < (int) sizeof(buf) - 1 ? req->content_len : (int) sizeof(buf) - 1;
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    cJSON *root = cJSON_Parse(buf);
    cJSON *confirm = root ? cJSON_GetObjectItemCaseSensitive(root, "confirm") : NULL;
    bool confirmed = confirm && cJSON_IsString(confirm) && strcmp(confirm->valuestring, "FORMATTA") == 0;
    cJSON_Delete(root);
    if (!confirmed) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "conferma mancante");
        return ESP_FAIL;
    }

    httpd_req_t *async_req = NULL;
    if (httpd_req_async_handler_begin(req, &async_req) != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "richiesta asincrona non disponibile");
        return ESP_FAIL;
    }
    if (xTaskCreate(sd_format_task, "sd_format", 6144, async_req, 3, NULL) != pdPASS) {
        httpd_resp_send_err(async_req, HTTPD_500_INTERNAL_SERVER_ERROR, "task di formattazione non avviato");
        httpd_req_async_handler_complete(async_req);
        return ESP_FAIL;
    }
    return ESP_OK;
}

static esp_err_t ota_check_online_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char version[32] = {0};
    char url[128] = {0};
    char msg[96] = {0};
    bool available = online_update_check(version, sizeof(version), url, sizeof(url), msg, sizeof(msg));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "available", available);
    cJSON_AddStringToObject(root, "version", version);
    cJSON_AddStringToObject(root, "url", url);
    cJSON_AddStringToObject(root, "message", msg);
    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t ota_apply_online_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char buf[513];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }
    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!url_item || !cJSON_IsString(url_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "campo \"url\" mancante");
        return ESP_FAIL;
    }

    char url[128];
    strncpy(url, url_item->valuestring, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    cJSON_Delete(root);

    // Avviato in un task separato: il download+applicazione puo' richiedere
    // decine di secondi, e serve poter interrogare l'avanzamento
    // (/api/ota/progress) mentre e' in corso invece di restare bloccati in
    // attesa di questa risposta HTTP fino alla fine.
    online_update_apply_async(url);
    ESP_LOGI(TAG, "Aggiornamento online avviato in background");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Avviato\"}");
    return ESP_OK;
}

static esp_err_t ota_progress_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    online_update_progress_t p = online_update_get_progress();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", p.running);
    cJSON_AddBoolToObject(root, "done", p.done);
    cJSON_AddBoolToObject(root, "ok", p.ok);
    cJSON_AddNumberToObject(root, "percent", p.percent);
    cJSON_AddNumberToObject(root, "bytes_read", p.bytes_read);
    cJSON_AddNumberToObject(root, "bytes_total", p.bytes_total);
    cJSON_AddStringToObject(root, "message", p.message);
    // L'albero cJSON va liberato prima dell'invio: tenerlo in memoria
    // insieme al testo e al buffer di rete sommava ~15 KB per richiesta.
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    // Allocato solo per la durata della richiesta: come buffer statico teneva
    // occupati 8 KB di RAM per sempre, per una pagina aperta di rado.
    char *buf = malloc(8192);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente, riprova");
    }
    size_t n = log_buffer_read(buf, 8192 - 1);
    buf[n] = '\0';

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, buf);
    free(buf);
    return ESP_OK;
}

static esp_err_t alerts_test_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    // Usa i valori correnti del form (non serve averli gia' salvati, stesso
    // principio del pulsante "Connetti" del WiFi) - un campo assente/vuoto
    // nel corpo mantiene il valore gia' salvato (vedi copy_field()).
    app_settings_t s = settings_get();

    if (req->content_len > 0) {
        if (req->content_len > 1024) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo troppo grande");
            return ESP_FAIL;
        }
        char *buf = malloc(req->content_len + 1);
        if (!buf) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria esaurita");
            return ESP_FAIL;
        }
        int received = 0;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, buf + received, req->content_len - received);
            if (r <= 0) {
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
                return ESP_FAIL;
            }
            received += r;
        }
        buf[received] = '\0';
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (root) {
            copy_field(root, "alert_smtp_host", s.alert_smtp_host, sizeof(s.alert_smtp_host));
            copy_field(root, "alert_smtp_user", s.alert_smtp_user, sizeof(s.alert_smtp_user));
            copy_field(root, "alert_smtp_password", s.alert_smtp_password, sizeof(s.alert_smtp_password));
            copy_field(root, "alert_email_to", s.alert_email_to, sizeof(s.alert_email_to));
            copy_field(root, "alert_whatsapp_phone", s.alert_whatsapp_phone, sizeof(s.alert_whatsapp_phone));
            copy_field(root, "alert_whatsapp_apikey", s.alert_whatsapp_apikey, sizeof(s.alert_whatsapp_apikey));
            cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "alert_smtp_port");
            if (port_item && cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535) {
                s.alert_smtp_port = (uint16_t) port_item->valueint;
            }
            cJSON_Delete(root);
        }
    }

    char msg[256] = {0};
    bool ok = alerts_send_test(&s, msg, sizeof(msg));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    cJSON_AddStringToObject(resp, "message", msg);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t ppp_log_start_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    bool ok = ppp_log_start();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false}");
    return ESP_OK;
}

static esp_err_t ppp_log_stop_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    ppp_log_stop();
    httpd_resp_set_type(req, "application/json");
    httpd_resp_set_hdr(req, "Cache-Control", "no-store, no-cache, must-revalidate");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Serve il file di log grezzo per il download dal browser, a blocchi (mai
// caricato tutto in RAM: una registrazione di diverse ore puo' superare
// facilmente la memoria disponibile) - fallisce con un messaggio chiaro se
// la registrazione e' ancora in corso (vedi ppp_log_open_for_read()) o se
// non c'e' nessun file salvato.
static esp_err_t ppp_log_download_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    FILE *f = ppp_log_open_for_read();
    if (!f) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        httpd_resp_sendstr(req, "Nessun log disponibile, oppure una registrazione e' ancora in corso (fermala prima di scaricare)");
        return ESP_OK;
    }

    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ppp_log.rtcm3\"");

    // Allocato solo durante lo scaricamento (prima 2 KB fissi).
    char *chunk = malloc(2048);
    size_t n;
    esp_err_t err = chunk ? ESP_OK : ESP_FAIL;
    while (chunk && (n = fread(chunk, 1, 2048, f)) > 0) {
        if (httpd_resp_send_chunk(req, chunk, n) != ESP_OK) {
            err = ESP_FAIL;
            break;
        }
    }
    free(chunk);
    httpd_resp_send_chunk(req, NULL, 0); // chiude la risposta chunked
    ppp_log_close_for_read(f);
    return err;
}

static esp_err_t wifi_forget_known_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    xSemaphoreTake(s_settings_edit_mutex, portMAX_DELAY);
    app_settings_t s = settings_get();
    memset(s.wifi_known_networks, 0, sizeof(s.wifi_known_networks));
    settings_save(&s);
    xSemaphoreGive(s_settings_edit_mutex);
    ESP_LOGI(TAG, "Elenco reti WiFi conosciute svuotato dalla UI web");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Misura della posizione base con RTK (base_measure.c): {"action":"start"}
// o {"action":"cancel"}. In entrambi i casi il dispositivo si riavvia.
static esp_err_t base_measure_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[64] = {0};
    int len = req->content_len < (int) sizeof(buf) - 1 ? req->content_len : (int) sizeof(buf) - 1;
    int received = 0;
    while (received < len) {
        int r = httpd_req_recv(req, buf + received, len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    cJSON *root = cJSON_Parse(buf);
    cJSON *action = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
    bool start = action && cJSON_IsString(action) && strcmp(action->valuestring, "start") == 0;
    bool cancel = action && cJSON_IsString(action) && strcmp(action->valuestring, "cancel") == 0;
    cJSON_Delete(root);

    char err[160] = {0};
    if (start) {
        if (!base_measure_request_start(err, sizeof(err))) {
            httpd_resp_set_type(req, "application/json");
            cJSON *resp = cJSON_CreateObject();
            cJSON_AddBoolToObject(resp, "ok", false);
            cJSON_AddStringToObject(resp, "error", err);
            char *json = cJSON_PrintUnformatted(resp);
            cJSON_Delete(resp);
            httpd_resp_sendstr(req, json ? json : "{\"ok\":false}");
            free(json);
            return ESP_OK;
        }
    } else if (cancel) {
        base_measure_request_cancel();
    } else {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "action deve essere start o cancel");
        return ESP_FAIL;
    }
    sys_stats_note_restart_reason("misura della posizione base (avvio o annullamento)");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

// Esportazione/importazione della configurazione (blob NVS, vedi
// settings_export_blob()). Contiene anche le password: resta un file
// dell'utente, come la copia sulla microSD.
static esp_err_t config_export_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    uint8_t *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    size_t len = settings_export_blob(buf, 4096);
    if (len == 0) {
        free(buf);
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "nessuna configurazione salvata");
    }
    char disp[80];
    snprintf(disp, sizeof(disp), "attachment; filename=\"evonetrtk_config_%s.bin\"", settings_get().device_serial);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    esp_err_t err = httpd_resp_send(req, (const char *) buf, len);
    free(buf);
    return err;
}

static esp_err_t config_import_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 4096) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "file di configurazione non valido");
    }
    uint8_t *buf = malloc(req->content_len);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    int got = 0;
    while (got < req->content_len) {
        int r = httpd_req_recv(req, (char *) buf + got, req->content_len - got);
        if (r <= 0) {
            free(buf);
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura file fallita");
        }
        got += r;
    }
    esp_err_t err = settings_import_blob(buf, (size_t) got, true);
    free(buf);
    if (err != ESP_OK) {
        httpd_resp_set_type(req, "application/json");
        return httpd_resp_sendstr(req, "{\"ok\":false,\"error\":\"Il file non e' una configurazione EVONETRTK valida\"}");
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    sys_stats_note_restart_reason("configurazione importata dal pannello");
    vTaskDelay(pdMS_TO_TICKS(500));
    esp_restart();
    return ESP_OK;
}

// Gestione della SIM (sim_tools.c). {"action": "credit" | "ussd" (code) |
// "sms" (number, text) | "read_sms" | "delete_sms" | "parse" (text)}.
// Le operazioni sul modem le esegue il task degli avvisi: la risposta arriva
// subito, l'esito poi in /api/status (sim_last_msg, sim_credit_*).
// "parse" prova soltanto la lettura dell'importo su un testo, senza modem.
// Prove delle protezioni da remoto (vedi sys_stats_test_fault): fanno
// riavviare la base apposta. Corpo: {"what":"freeze_gnss"} ecc.
static esp_err_t test_fault_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[64] = {0};
    int len = req->content_len < (int) sizeof(buf) - 1 ? req->content_len : (int) sizeof(buf) - 1;
    if (len <= 0 || httpd_req_recv(req, buf, len) != len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo mancante");
    }
    cJSON *root = cJSON_Parse(buf);
    const cJSON *w = root ? cJSON_GetObjectItemCaseSensitive(root, "what") : NULL;
    char what[24] = "";
    if (w && cJSON_IsString(w)) {
        strlcpy(what, w->valuestring, sizeof(what));
    }
    cJSON_Delete(root);
    httpd_resp_set_type(req, "application/json");
    if (strcmp(what, "save_good") == 0) {
        config_backup_save_good_now();
        return httpd_resp_sendstr(req, "{\"ok\":true}");
    }
    if (strcmp(what, "gnss_reset") == 0) {
        httpd_resp_sendstr(req, "{\"ok\":true}"); // poi la base si riavvia
        sys_stats_test_fault(what);
        return ESP_OK;
    }
    bool ok = sys_stats_test_fault(what);
    return httpd_resp_sendstr(req, ok ? "{\"ok\":true}" : "{\"ok\":false,\"error\":\"prova sconosciuta\"}");
}

// Nuova coppia di chiavi VPN: la privata resta sulla base, si restituisce
// solo la pubblica (da mettere sul server WireGuard).
static esp_err_t vpn_keygen_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char pub[48] = "";
    bool ok = vpn_link_generate_keys(pub, sizeof(pub));
    char out[96];
    snprintf(out, sizeof(out), ok ? "{\"ok\":true,\"public_key\":\"%s\"}" : "{\"ok\":false}", pub);
    httpd_resp_set_type(req, "application/json");
    return httpd_resp_sendstr(req, out);
}

// Registrazione dei dati grezzi: {"action":"start","hours":24,"interval":5},
// {"action":"stop"}, {"action":"delete","name":"EVO_....ubx"}.
static esp_err_t rawlog_action_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[160] = {0};
    int len = req->content_len < (int) sizeof(buf) - 1 ? req->content_len : (int) sizeof(buf) - 1;
    if (len <= 0 || httpd_req_recv(req, buf, len) != len) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo mancante");
    }
    cJSON *root = cJSON_Parse(buf);
    const cJSON *a = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
    const char *action = (a && cJSON_IsString(a)) ? a->valuestring : "";
    char err[96] = "";
    bool ok = false;
    if (strcmp(action, "start") == 0) {
        const cJSON *h = cJSON_GetObjectItemCaseSensitive(root, "hours");
        const cJSON *iv = cJSON_GetObjectItemCaseSensitive(root, "interval");
        ok = raw_log_start(cJSON_IsNumber(h) ? (uint32_t) h->valueint : 0,
                           cJSON_IsNumber(iv) ? (uint8_t) iv->valueint : 0, err, sizeof(err));
    } else if (strcmp(action, "stop") == 0) {
        raw_log_stop("fermata dal pannello");
        ok = true;
    } else if (strcmp(action, "delete") == 0) {
        const cJSON *n = cJSON_GetObjectItemCaseSensitive(root, "name");
        ok = n && cJSON_IsString(n) && raw_log_delete(n->valuestring);
        if (!ok) {
            strlcpy(err, "file non cancellato (inesistente o in registrazione)", sizeof(err));
        }
    } else {
        strlcpy(err, "azione sconosciuta", sizeof(err));
    }
    cJSON_Delete(root);
    cJSON *out = cJSON_CreateObject();
    cJSON_AddBoolToObject(out, "ok", ok);
    if (!ok) {
        cJSON_AddStringToObject(out, "error", err);
    }
    char *json = cJSON_PrintUnformatted(out);
    cJSON_Delete(out);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, json ? json : "{\"ok\":false}");
    free(json);
    return e;
}

static esp_err_t rawlog_list_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    return raw_log_send_list(req);
}

static esp_err_t rawlog_download_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char query[96] = {0}, name[48] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "name", name, sizeof(name)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "manca ?name=");
    }
    return raw_log_send_file(req, name);
}

static esp_err_t sim_action_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char buf[256] = {0};
    int len = req->content_len < (int) sizeof(buf) - 1 ? req->content_len : (int) sizeof(buf) - 1;
    int got = 0;
    while (got < len) {
        int r = httpd_req_recv(req, buf + got, len - got);
        if (r <= 0) {
            return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
        }
        got += r;
    }
    cJSON *root = cJSON_Parse(buf);
    const cJSON *act = root ? cJSON_GetObjectItemCaseSensitive(root, "action") : NULL;
    const char *a = act && cJSON_IsString(act) ? act->valuestring : "";
    const cJSON *jc = root ? cJSON_GetObjectItemCaseSensitive(root, "code") : NULL;
    const cJSON *jn = root ? cJSON_GetObjectItemCaseSensitive(root, "number") : NULL;
    const cJSON *jt = root ? cJSON_GetObjectItemCaseSensitive(root, "text") : NULL;
    const char *code = jc && cJSON_IsString(jc) ? jc->valuestring : "";
    const char *num = jn && cJSON_IsString(jn) ? jn->valuestring : "";
    const char *txt = jt && cJSON_IsString(jt) ? jt->valuestring : "";

    cJSON *resp = cJSON_CreateObject();
    char err[96] = "";
    bool ok = false;
    if (strcmp(a, "parse") == 0) {
        float eur;
        ok = sim_tools_parse_euro(txt, &eur);
        if (ok) {
            cJSON_AddNumberToObject(resp, "eur", eur);
        }
    } else if (strcmp(a, "credit") == 0) {
        ok = sim_tools_request(SIM_ACT_CREDIT, NULL, NULL, err, sizeof(err));
    } else if (strcmp(a, "ussd") == 0 && code[0]) {
        ok = sim_tools_request(SIM_ACT_USSD, code, NULL, err, sizeof(err));
    } else if (strcmp(a, "sms") == 0 && num[0] && txt[0]) {
        ok = sim_tools_request(SIM_ACT_SMS, num, txt, err, sizeof(err));
    } else if (strcmp(a, "read_sms") == 0) {
        ok = sim_tools_request(SIM_ACT_READ_SMS, NULL, NULL, err, sizeof(err));
    } else if (strcmp(a, "delete_sms") == 0) {
        ok = sim_tools_request(SIM_ACT_DELETE_SMS, NULL, NULL, err, sizeof(err));
    } else {
        snprintf(err, sizeof(err), "Richiesta non valida o campi mancanti");
    }
    cJSON_Delete(root);
    cJSON_AddBoolToObject(resp, "ok", ok);
    if (err[0]) {
        cJSON_AddStringToObject(resp, "error", err);
    }
    char *json = cJSON_PrintUnformatted(resp);
    cJSON_Delete(resp);
    httpd_resp_set_type(req, "application/json");
    esp_err_t e = httpd_resp_sendstr(req, json ? json : "{\"ok\":false}");
    free(json);
    return e;
}

static esp_err_t sim_sms_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char *buf = malloc(2048);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    sim_tools_get_sms(buf, 2048);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    esp_err_t e = httpd_resp_sendstr(req, buf);
    free(buf);
    return e;
}

// Log salvati sulla microSD: elenco e scaricamento (diag_log.c).
static esp_err_t diag_list_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    diag_log_file_t files[5];
    int n = diag_log_list(files, 5);
    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "sd_ok", n >= 0);
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    for (int i = 0; i < n; i++) {
        cJSON *o = cJSON_CreateObject();
        cJSON_AddNumberToObject(o, "index", files[i].index);
        cJSON_AddNumberToObject(o, "size", files[i].size);
        cJSON_AddBoolToObject(o, "current", files[i].current);
        cJSON_AddItemToArray(arr, o);
    }
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    return ESP_OK;
}

static esp_err_t diag_download_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    char query[32] = {0}, val[8] = {0};
    if (httpd_req_get_url_query_str(req, query, sizeof(query)) != ESP_OK ||
        httpd_query_key_value(query, "n", val, sizeof(val)) != ESP_OK) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "manca ?n=");
    }
    return diag_log_send_http(req, atoi(val));
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Riavvio richiesto dalla UI web");
    sys_stats_note_restart_reason("richiesto dal pannello");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

void web_ui_start(void)
{
    s_settings_edit_mutex = xSemaphoreCreateMutex();

    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    // default 8, non basta piu' con tutti gli endpoint aggiunti (23 in uso
    // in questa versione) - portato a 32 invece che al minimo esatto
    // (avrebbe lasciato solo 1 slot libero): httpd_register_uri_handler()
    // qui sotto non controlla il valore di ritorno, un nuovo endpoint
    // aggiunto in futuro senza alzare anche questo numero fallirebbe in
    // silenzio (nessun log, nessun crash, solo un 404 inspiegabile su
    // quell'endpoint) - trovato rivedendo il codice, non ancora capitato
    // in pratica.
    config.max_uri_handlers = 48; // 41 in uso con la registrazione dei dati grezzi (1.19.124)
    // Con il default (7 socket, nessuna chiusura automatica) bastavano un
    // paio di browser/schede aperte, che tengono le connessioni in
    // keep-alive e interrogano /api/status e /api/signals ogni pochi
    // secondi, per esaurire i socket: ogni nuova connessione veniva
    // rifiutata e il pannello sembrava morto per minuti (visto sul
    // dispositivo). Con lru_purge il socket usato meno di recente viene
    // chiuso per fare posto al nuovo.
    config.lru_purge_enable = true;
    // Il default (4096 byte) va in overflow quando un handler fa una
    // richiesta HTTPS in uscita (es. ota_check_online_post_handler verso
    // GitHub): l'handshake TLS/mbedTLS richiede piu' stack di quanto ne
    // serva per gestire richieste normali - confermato da un crash reale
    // su hardware (stessa causa, in un task diverso, del fix allo stack
    // del task "main" durante l'init WiFi fatto in precedenza).
    config.stack_size = 14336; // 14 KB (era 12): con scheda di accesso, VPN e caster restavano 1092 byte liberi (04/10/2026)

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Avvio server web fallito");
        return;
    }

    httpd_uri_t index_uri      = { .uri = "/",              .method = HTTP_GET,  .handler = index_get_handler };
    httpd_uri_t icon_uri       = { .uri = "/icon192.png",   .method = HTTP_GET,  .handler = icon_get_handler };
    httpd_uri_t manifest_uri   = { .uri = "/manifest.json", .method = HTTP_GET,  .handler = manifest_get_handler };
    httpd_uri_t status_uri     = { .uri = "/api/status",     .method = HTTP_GET,  .handler = status_get_handler };
    httpd_uri_t signals_uri    = { .uri = "/api/signals",    .method = HTTP_GET,  .handler = signals_get_handler };
    httpd_uri_t settings_uri   = { .uri = "/api/settings",   .method = HTTP_POST, .handler = settings_post_handler };
    httpd_uri_t reboot_uri     = { .uri = "/api/reboot",     .method = HTTP_POST, .handler = reboot_post_handler };
    httpd_uri_t cfg_export_uri = { .uri = "/api/config/export", .method = HTTP_GET, .handler = config_export_get_handler };
    httpd_uri_t cfg_import_uri = { .uri = "/api/config/import", .method = HTTP_POST, .handler = config_import_post_handler };
    httpd_uri_t sim_action_uri = { .uri = "/api/sim/action", .method = HTTP_POST, .handler = sim_action_post_handler };
    httpd_uri_t test_fault_uri = { .uri = "/api/test/fault", .method = HTTP_POST, .handler = test_fault_post_handler };
    httpd_uri_t access_uri     = { .uri = "/access", .method = HTTP_GET, .handler = access_get_handler };
    httpd_uri_t access_sec_uri = { .uri = "/api/access/secrets", .method = HTTP_POST, .handler = access_secrets_post_handler };
    httpd_uri_t vpn_keygen_uri = { .uri = "/api/vpn/keygen", .method = HTTP_POST, .handler = vpn_keygen_post_handler };
    httpd_uri_t rawlog_action_uri = { .uri = "/api/rawlog/action", .method = HTTP_POST, .handler = rawlog_action_post_handler };
    httpd_uri_t rawlog_list_uri   = { .uri = "/api/rawlog/list", .method = HTTP_GET, .handler = rawlog_list_get_handler };
    httpd_uri_t rawlog_dl_uri     = { .uri = "/api/rawlog/download", .method = HTTP_GET, .handler = rawlog_download_get_handler };
    httpd_uri_t sim_sms_uri    = { .uri = "/api/sim/sms",    .method = HTTP_GET,  .handler = sim_sms_get_handler };
    httpd_uri_t diag_list_uri  = { .uri = "/api/diag/list",     .method = HTTP_GET, .handler = diag_list_get_handler };
    httpd_uri_t diag_dl_uri    = { .uri = "/api/diag/download", .method = HTTP_GET, .handler = diag_download_get_handler };
    httpd_uri_t base_measure_uri = { .uri = "/api/base/measure", .method = HTTP_POST, .handler = base_measure_post_handler };
    httpd_uri_t ota_upload_uri = { .uri = "/api/ota/upload",    .method = HTTP_POST, .handler = ota_upload_post_handler };
    httpd_uri_t ota_sd_uri     = { .uri = "/api/ota/sd-update", .method = HTTP_POST, .handler = ota_sd_post_handler };
    httpd_uri_t sd_format_uri  = { .uri = "/api/sd/format",     .method = HTTP_POST, .handler = sd_format_post_handler };
    httpd_uri_t ota_check_uri  = { .uri = "/api/ota/check-online", .method = HTTP_POST, .handler = ota_check_online_post_handler };
    httpd_uri_t ota_apply_uri  = { .uri = "/api/ota/apply-online", .method = HTTP_POST, .handler = ota_apply_online_post_handler };
    httpd_uri_t ota_progress_uri = { .uri = "/api/ota/progress", .method = HTTP_GET, .handler = ota_progress_get_handler };
    httpd_uri_t wifi_scan_uri  = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler };
    httpd_uri_t ntrip_mountpoints_uri = { .uri = "/api/ntrip/mountpoints", .method = HTTP_GET, .handler = ntrip_mountpoints_get_handler };
    httpd_uri_t ntrip_test_uri = { .uri = "/api/ntrip/test-connect", .method = HTTP_POST, .handler = ntrip_test_connect_post_handler };
    httpd_uri_t wifi_test_uri  = { .uri = "/api/wifi/test-connect", .method = HTTP_POST, .handler = wifi_test_connect_post_handler };
    httpd_uri_t wifi_test_progress_uri = { .uri = "/api/wifi/test-progress", .method = HTTP_GET, .handler = wifi_test_progress_get_handler };
    httpd_uri_t wifi_forget_uri = { .uri = "/api/wifi/forget-known", .method = HTTP_POST, .handler = wifi_forget_known_post_handler };
    httpd_uri_t log_uri        = { .uri = "/api/log", .method = HTTP_GET, .handler = log_get_handler };
    httpd_uri_t alerts_test_uri = { .uri = "/api/alerts/test", .method = HTTP_POST, .handler = alerts_test_post_handler };
    httpd_uri_t ppp_log_start_uri = { .uri = "/api/ppp-log/start", .method = HTTP_POST, .handler = ppp_log_start_post_handler };
    httpd_uri_t ppp_log_stop_uri  = { .uri = "/api/ppp-log/stop",  .method = HTTP_POST, .handler = ppp_log_stop_post_handler };
    httpd_uri_t ppp_log_dl_uri    = { .uri = "/api/ppp-log/download", .method = HTTP_GET, .handler = ppp_log_download_get_handler };
    httpd_uri_t fw_archive_list_uri  = { .uri = "/api/firmware-archive/list",  .method = HTTP_GET,  .handler = fw_archive_list_get_handler };
    httpd_uri_t fw_archive_apply_uri = { .uri = "/api/firmware-archive/apply", .method = HTTP_POST, .handler = fw_archive_apply_post_handler };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &icon_uri);
    httpd_register_uri_handler(server, &manifest_uri);
    httpd_register_uri_handler(server, &wifi_scan_uri);
    httpd_register_uri_handler(server, &test_fault_uri);
    httpd_register_uri_handler(server, &access_uri);
    httpd_register_uri_handler(server, &access_sec_uri);
    httpd_register_uri_handler(server, &vpn_keygen_uri);
    httpd_register_uri_handler(server, &rawlog_action_uri);
    httpd_register_uri_handler(server, &rawlog_list_uri);
    httpd_register_uri_handler(server, &rawlog_dl_uri);
    httpd_register_uri_handler(server, &ntrip_mountpoints_uri);
    httpd_register_uri_handler(server, &ntrip_test_uri);
    httpd_register_uri_handler(server, &wifi_test_uri);
    httpd_register_uri_handler(server, &wifi_test_progress_uri);
    httpd_register_uri_handler(server, &wifi_forget_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &signals_uri);
    httpd_register_uri_handler(server, &settings_uri);
    httpd_register_uri_handler(server, &reboot_uri);
    httpd_register_uri_handler(server, &base_measure_uri);
    httpd_register_uri_handler(server, &diag_list_uri);
    httpd_register_uri_handler(server, &sim_action_uri);
    httpd_register_uri_handler(server, &sim_sms_uri);
    httpd_register_uri_handler(server, &cfg_export_uri);
    httpd_register_uri_handler(server, &cfg_import_uri);
    httpd_register_uri_handler(server, &diag_dl_uri);
    httpd_register_uri_handler(server, &ota_upload_uri);
    httpd_register_uri_handler(server, &ota_sd_uri);
    httpd_register_uri_handler(server, &sd_format_uri);
    httpd_register_uri_handler(server, &ota_check_uri);
    httpd_register_uri_handler(server, &ota_apply_uri);
    httpd_register_uri_handler(server, &ota_progress_uri);
    httpd_register_uri_handler(server, &log_uri);
    httpd_register_uri_handler(server, &alerts_test_uri);
    httpd_register_uri_handler(server, &ppp_log_start_uri);
    httpd_register_uri_handler(server, &ppp_log_stop_uri);
    httpd_register_uri_handler(server, &ppp_log_dl_uri);
    httpd_register_uri_handler(server, &fw_archive_list_uri);
    httpd_register_uri_handler(server, &fw_archive_apply_uri);

    ESP_LOGI(TAG, "Server web di gestione avviato");
}
