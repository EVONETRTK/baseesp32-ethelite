#include "online_update.h"
#include "version.h"
#include "ota_update.h"
#include "settings.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "online_update";

// Accumula il corpo della risposta HTTP (il manifest JSON, sempre
// piccolo) in un buffer a dimensione fissa fornito dal chiamante, e
// l'header Location se presente. Quest'ultimo va catturato qui, non con
// esp_http_client_get_header() dopo esp_http_client_perform(): confermato
// su hardware reale che quella chiamata non trova l'header su una
// risposta 302 di GitHub nonostante l'header sia effettivamente presente
// (la richiesta funziona, arriva un 302 genuino - solo la lettura post
// hoc dell'header fallisce).
// Gli URL firmati verso cui GitHub reindirizza (Azure Blob Storage, con
// token SAS + un JWT completo nella query string) sono MOLTO piu' lunghi
// del previsto - confermato su hardware reale che perfino 600 byte li
// troncano a meta' del JWT, producendo una richiesta malformata (status
// di risposta non valido, corpo vuoto/illeggibile). 1536 da margine
// ampio anche per URL insolitamente lunghi.
#define MAX_URL_LEN 1536

typedef struct {
    char *buf;
    size_t size;
    size_t used;
    char location[MAX_URL_LEN];
} http_download_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_download_ctx_t *ctx = (http_download_ctx_t *) evt->user_data;
    if (!ctx) {
        return ESP_OK;
    }
    if (evt->event_id == HTTP_EVENT_ON_HEADER) {
        if (evt->header_key && strcasecmp(evt->header_key, "Location") == 0 && evt->header_value) {
            strncpy(ctx->location, evt->header_value, sizeof(ctx->location) - 1);
            ctx->location[sizeof(ctx->location) - 1] = '\0';
        }
    } else if (evt->event_id == HTTP_EVENT_ON_DATA && ctx->used + 1 < ctx->size) {
        size_t space = ctx->size - ctx->used - 1;
        size_t n = (size_t) evt->data_len < space ? (size_t) evt->data_len : space;
        memcpy(ctx->buf + ctx->used, evt->data, n);
        ctx->used += n;
        ctx->buf[ctx->used] = '\0';
    }
    return ESP_OK;
}

#define MAX_REDIRECTS 3

// Esegue una richiesta seguendo manualmente fino a MAX_REDIRECTS
// redirect (header Location) - il redirect automatico di
// esp_http_client si e' dimostrato inaffidabile su hardware reale verso
// gli URL "latest" di GitHub Releases (confermato: "err=ESP_FAIL
// status=302" nonostante disable_auto_redirect non fosse impostato).
// use_head = true evita di scaricare il corpo quando serve solo risolvere
// l'URL finale (es. prima di passarlo a esp_https_ota()). Se body_buf non
// e' NULL, vi copia il corpo della risposta finale. out_final_url (se non
// NULL) riceve l'URL della risposta finale. Ritorna lo status HTTP finale,
// o <0 in caso di errore di rete.
static int http_fetch_following_redirects(const char *url, bool use_head,
                                           char *out_final_url, size_t out_final_url_size,
                                           char *body_buf, size_t body_buf_size)
{
    char current_url[MAX_URL_LEN];
    strncpy(current_url, url, sizeof(current_url) - 1);
    current_url[sizeof(current_url) - 1] = '\0';

    for (int hop = 0; hop < MAX_REDIRECTS; hop++) {
        http_download_ctx_t ctx = { .buf = body_buf, .size = body_buf_size, .used = 0 };
        if (body_buf) {
            body_buf[0] = '\0';
        }

        esp_http_client_config_t config = {
            .url = current_url,
            .method = use_head ? HTTP_METHOD_HEAD : HTTP_METHOD_GET,
            .event_handler = http_event_handler,
            .user_data = &ctx,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .disable_auto_redirect = true,
            .buffer_size = 2048,    // margine per intestazioni lunghe (URL firmati con token SAS)
            .buffer_size_tx = 2048,
        };
        ESP_LOGI(TAG, "Hop %d: %s %.100s%s", hop + 1, use_head ? "HEAD" : "GET",
                 current_url, strlen(current_url) > 100 ? "..." : "");

        esp_http_client_handle_t client = esp_http_client_init(&config);
        // Il CDN di GitHub Releases risponde a volte con corpo compresso
        // (gzip) anche senza che il client lo richieda - esp_http_client
        // non lo decomprime da solo, risultato: byte ricevuti coerenti
        // in numero ma illeggibili come testo (confermato su hardware:
        // 457 byte "ricevuti" ma vuoti/non stampabili). Si richiede
        // esplicitamente contenuto non compresso.
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        esp_err_t err = esp_http_client_perform(client);
        int status = esp_http_client_get_status_code(client);
        int64_t content_len = esp_http_client_get_content_length(client);
        esp_http_client_cleanup(client);

        // Log incondizionato ad ogni hop (successo o no), non solo sul
        // fallimento finale: senza questo, una cattura del log che parte
        // a meta' sequenza mostra solo l'ultimo esito senza il contesto
        // di come ci si e' arrivati (successo/fallimento precedenti,
        // quanti hop, redirect trovati o no).
        ESP_LOGI(TAG, "Hop %d esito: err=%s status=%d content_len=%lld location=%s",
                 hop + 1, esp_err_to_name(err), status, (long long) content_len,
                 ctx.location[0] ? ctx.location : "(assente)");

        if (err != ESP_OK) {
            ESP_LOGW(TAG, "Richiesta a %s fallita: %s", current_url, esp_err_to_name(err));
            return -1;
        }

        if (status >= 300 && status < 400) {
            if (ctx.location[0] == '\0') {
                ESP_LOGW(TAG, "Redirect (status %d) senza header Location", status);
                return status;
            }
            strncpy(current_url, ctx.location, sizeof(current_url) - 1);
            current_url[sizeof(current_url) - 1] = '\0';
            ESP_LOGI(TAG, "Redirect (hop %d) -> %s", hop + 1, current_url);
            continue;
        }

        if (out_final_url) {
            strncpy(out_final_url, current_url, out_final_url_size - 1);
            out_final_url[out_final_url_size - 1] = '\0';
        }
        return status;
    }

    ESP_LOGW(TAG, "Troppi redirect (>%d) per %s", MAX_REDIRECTS, url);
    return -1;
}

bool online_update_check(char *out_version, size_t out_version_size,
                          char *out_url, size_t out_url_size,
                          char *out_msg, size_t out_msg_size)
{
#define SET_MSG(...) do { if (out_msg) snprintf(out_msg, out_msg_size, __VA_ARGS__); } while (0)

    app_settings_t settings = settings_get();
    if (strlen(settings.ota_update_url) == 0) {
        SET_MSG("Nessun indirizzo di aggiornamento online configurato");
        return false;
    }

    char manifest[512] = {0};
    int status = http_fetch_following_redirects(settings.ota_update_url, false, NULL, 0, manifest, sizeof(manifest));
    ESP_LOGI(TAG, "Esito finale: status=%d, %d byte ricevuti", status, (int) strlen(manifest));

    // Non ci si affida allo status HTTP come unico segnale di successo:
    // confermato su hardware reale che il CDN di GitHub Releases
    // (release-assets.githubusercontent.com) puo' restituire un codice
    // non valido/non standard (es. 618) pur avendo trasferito il
    // contenuto correttamente (byte ricevuti coerenti col file reale,
    // err di trasporto ESP_OK) - probabile limite del parser dello status
    // in esp_http_client su risposte di questo servizio specifico. Se il
    // corpo e' JSON valido lo consideriamo comunque un successo.
    if (strlen(manifest) == 0) {
        ESP_LOGW(TAG, "Controllo aggiornamenti fallito: nessun contenuto ricevuto (status=%d)", status);
        SET_MSG("Impossibile raggiungere l'indirizzo di aggiornamento configurato");
        return false;
    }

    cJSON *root = cJSON_Parse(manifest);
    if (!root) {
        ESP_LOGW(TAG, "Risposta non valida (status=%d): %.100s", status, manifest);
        SET_MSG("Risposta non valida (JSON) dal server di aggiornamento");
        return false;
    }

    cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!ver_item || !cJSON_IsString(ver_item)) {
        cJSON_Delete(root);
        SET_MSG("Manifest senza campo \"version\"");
        return false;
    }

    if (out_version) {
        strncpy(out_version, ver_item->valuestring, out_version_size - 1);
        out_version[out_version_size - 1] = '\0';
    }

    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (out_url) {
        if (url_item && cJSON_IsString(url_item)) {
            strncpy(out_url, url_item->valuestring, out_url_size - 1);
            out_url[out_url_size - 1] = '\0';
        } else {
            out_url[0] = '\0';
        }
    }

    bool newer = ota_update_semver_compare(ver_item->valuestring, FIRMWARE_VERSION) > 0;
    if (newer) {
        SET_MSG("Nuova versione disponibile: %s (attuale: %s)", ver_item->valuestring, FIRMWARE_VERSION);
    } else {
        SET_MSG("Gia' aggiornato (attuale: %s, online: %s)", FIRMWARE_VERSION, ver_item->valuestring);
    }
    cJSON_Delete(root);
    return newer;

#undef SET_MSG
}

bool online_update_apply(const char *firmware_url, char *out_msg, size_t out_msg_size)
{
#define SET_MSG(...) do { if (out_msg) snprintf(out_msg, out_msg_size, __VA_ARGS__); } while (0)

    if (!firmware_url || strlen(firmware_url) == 0) {
        SET_MSG("Nessun indirizzo del firmware da scaricare");
        return false;
    }

    // Risolve prima eventuali redirect (es. GitHub Releases "latest") a
    // mano con una HEAD, cosi' esp_https_ota() sotto riceve gia' l'URL
    // finale e non deve seguirne lui stesso (stesso motivo del redirect
    // manuale in online_update_check() sopra).
    // Non ci si affida allo status HTTP (vedi il commento in
    // online_update_check() sopra: il CDN di GitHub Releases puo'
    // restituire uno status non valido pur avendo risposto correttamente)
    // - un errore di trasporto (return -1) resta l'unico segnale di
    // fallimento affidabile qui.
    char resolved_url[MAX_URL_LEN];
    int status = http_fetch_following_redirects(firmware_url, true, resolved_url, sizeof(resolved_url), NULL, 0);
    if (status < 0) {
        ESP_LOGW(TAG, "Impossibile risolvere l'URL del firmware: errore di trasporto");
        SET_MSG("Indirizzo del firmware non raggiungibile");
        return false;
    }
    ESP_LOGI(TAG, "URL firmware risolto (status=%d): %.100s", status, resolved_url);

    esp_http_client_config_t http_config = {
        .url = resolved_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
        .buffer_size = 2048,
        .buffer_size_tx = 2048,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Download aggiornamento online da %s...", resolved_url);
    esp_err_t err = esp_https_ota(&ota_config);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Aggiornamento online fallito: %s", esp_err_to_name(err));
        SET_MSG("Aggiornamento fallito, firmware attuale non modificato");
        return false;
    }

    SET_MSG("Aggiornamento scaricato e applicato, riavvio...");
    return true;

#undef SET_MSG
}
