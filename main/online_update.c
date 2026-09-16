#include "online_update.h"
#include "version.h"
#include "ota_update.h"
#include "settings.h"
#include "fw_archive.h"
#include "status.h"

#include <string.h>
#include <strings.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "online_update";

// Stato di avanzamento del download/applicazione, letto dalla UI web con
// online_update_get_progress() mentre online_update_apply_async() gira in
// un task separato - protetto da mutex perche' scritto dal task di
// aggiornamento e letto dal task del server web in parallelo.
static SemaphoreHandle_t s_progress_mutex;
static online_update_progress_t s_progress = { .percent = -1, .bytes_total = -1 };

static void progress_lock_init(void)
{
    if (!s_progress_mutex) {
        s_progress_mutex = xSemaphoreCreateMutex();
    }
}

static void progress_reset(void)
{
    progress_lock_init();
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress = (online_update_progress_t){ .running = true, .percent = -1, .bytes_total = -1 };
    xSemaphoreGive(s_progress_mutex);
}

static void progress_set_bytes(int read, int total)
{
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress.bytes_read = read;
    s_progress.bytes_total = total;
    s_progress.percent = (total > 0) ? (int) ((int64_t) read * 100 / total) : -1;
    xSemaphoreGive(s_progress_mutex);
}

static void progress_finish(bool ok, const char *msg)
{
    progress_lock_init();
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    s_progress.running = false;
    s_progress.done = true;
    s_progress.ok = ok;
    if (ok) {
        s_progress.percent = 100;
    }
    if (msg) {
        strncpy(s_progress.message, msg, sizeof(s_progress.message) - 1);
        s_progress.message[sizeof(s_progress.message) - 1] = '\0';
    }
    xSemaphoreGive(s_progress_mutex);
}

online_update_progress_t online_update_get_progress(void)
{
    if (!s_progress_mutex) {
        return (online_update_progress_t){0};
    }
    online_update_progress_t copy;
    xSemaphoreTake(s_progress_mutex, portMAX_DELAY);
    copy = s_progress;
    xSemaphoreGive(s_progress_mutex);
    return copy;
}

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
// minimal_range = true evita di scaricare il corpo intero quando serve solo
// risolvere l'URL finale (es. prima di passarlo a esp_https_ota()) - NON
// si puo' usare HEAD per questo: confermato su hardware reale che
// l'endpoint di redirect degli asset di GitHub Releases
// (/releases/download/TAG/file) risponde "404 File not found" a una
// richiesta HEAD, pur reindirizzando correttamente con GET (lo stesso
// metodo gia' usato con successo per il manifest JSON). Si usa quindi
// sempre GET, con un header Range che limita il trasferimento a un solo
// byte quando non serve il corpo.
// Se body_buf non e' NULL, vi copia il corpo della risposta finale.
// out_final_url (se non NULL) riceve l'URL della risposta finale. Ritorna
// lo status HTTP finale, o <0 in caso di errore di rete.
static int http_fetch_following_redirects(const char *url, bool minimal_range,
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
            .method = HTTP_METHOD_GET,
            .event_handler = http_event_handler,
            .user_data = &ctx,
            .crt_bundle_attach = esp_crt_bundle_attach,
            .timeout_ms = 10000,
            .disable_auto_redirect = true,
            .buffer_size = 2048,    // margine per intestazioni lunghe (URL firmati con token SAS)
            .buffer_size_tx = 2048,
        };
        ESP_LOGI(TAG, "Hop %d: GET%s %.100s%s", hop + 1, minimal_range ? " (range 0-0)" : "",
                 current_url, strlen(current_url) > 100 ? "..." : "");

        esp_http_client_handle_t client = esp_http_client_init(&config);
        // Il CDN di GitHub Releases risponde a volte con corpo compresso
        // (gzip) anche senza che il client lo richieda - esp_http_client
        // non lo decomprime da solo, risultato: byte ricevuti coerenti
        // in numero ma illeggibili come testo (confermato su hardware:
        // 457 byte "ricevuti" ma vuoti/non stampabili). Si richiede
        // esplicitamente contenuto non compresso.
        esp_http_client_set_header(client, "Accept-Encoding", "identity");
        if (minimal_range) {
            esp_http_client_set_header(client, "Range", "bytes=0-0");
        }
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

    // Da qui in poi il controllo e' riuscito davvero (manifest valido
    // ricevuto e decodificato) - non conta un tentativo mai arrivato a
    // buon fine per rete assente/irraggiungibile (i return false sopra).
    status_note_online_update_checked();

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

// Passato a esp_https_ota_begin(): applica lo stesso fix gia' confermato
// necessario in http_fetch_following_redirects() sopra (Accept-Encoding:
// identity). esp_https_ota() usa un client HTTP interno che non condivide
// codice con quella funzione, quindi senza questo header rischia lo stesso
// problema di contenuto compresso non atteso - qui pero' scriverebbe i
// byte (sbagliati) direttamente sulla partizione OTA, con l'immagine che
// fallisce la validazione invece di un JSON illeggibile.
static esp_err_t ota_http_client_init_cb(esp_http_client_handle_t http_client)
{
    esp_http_client_set_header(http_client, "Accept-Encoding", "identity");
    return ESP_OK;
}

bool online_update_apply(const char *firmware_url, char *out_msg, size_t out_msg_size)
{
#define SET_MSG(...) do { if (out_msg) snprintf(out_msg, out_msg_size, __VA_ARGS__); } while (0)
#define FAIL(...) do { SET_MSG(__VA_ARGS__); progress_finish(false, out_msg); return false; } while (0)

    // Best-effort, non blocca l'aggiornamento se la SD non e' disponibile
    // (vedi fw_archive.h) - questo percorso (esp_https_ota diretto) non
    // passa da ota_update_apply() in ota_update.c, quindi l'archiviazione
    // va richiamata qui esplicitamente per coprire anche gli aggiornamenti
    // online/automatici, non solo upload da browser e SD.
    fw_archive_save_current();

    progress_reset();

    if (!firmware_url || strlen(firmware_url) == 0) {
        FAIL("Nessun indirizzo del firmware da scaricare");
    }

    // Risolve prima eventuali redirect (es. GitHub Releases "latest") a
    // mano (range minimo, non HEAD - vedi commento su
    // http_fetch_following_redirects() sopra), cosi' esp_https_ota() sotto
    // riceve gia' l'URL finale e non deve seguirne lui stesso (stesso
    // motivo del redirect manuale in online_update_check() sopra).
    // Non ci si affida allo status HTTP (vedi il commento in
    // online_update_check() sopra: il CDN di GitHub Releases puo'
    // restituire uno status non valido pur avendo risposto correttamente)
    // - un errore di trasporto (return -1) resta l'unico segnale di
    // fallimento affidabile qui.
    char resolved_url[MAX_URL_LEN];
    int status = http_fetch_following_redirects(firmware_url, true, resolved_url, sizeof(resolved_url), NULL, 0);
    if (status < 0) {
        ESP_LOGW(TAG, "Impossibile risolvere l'URL del firmware: errore di trasporto");
        FAIL("Indirizzo del firmware non raggiungibile");
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
        .http_client_init_cb = ota_http_client_init_cb,
    };

    esp_https_ota_handle_t handle = NULL;
    esp_err_t err = esp_https_ota_begin(&ota_config, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_https_ota_begin fallito: %s", esp_err_to_name(err));
        FAIL("Aggiornamento fallito, avvio del download non riuscito");
    }

    // Dimensione totale nota dal Content-Length della risposta, se il CDN
    // la fornisce - usata solo per calcolare la percentuale mostrata nella
    // UI web; -1 (sconosciuta) non blocca comunque il download.
    int total = esp_https_ota_get_image_size(handle);
    ESP_LOGI(TAG, "Download aggiornamento online da %s (dimensione %s: %d byte)...",
             resolved_url, total > 0 ? "nota" : "sconosciuta", total);

    // API "a passi" invece della singola esp_https_ota(): permette di
    // leggere quanti byte sono stati scaricati mano a mano
    // (esp_https_ota_get_image_len_read()) e pubblicarli in s_progress per
    // la barra di avanzamento nella UI web, cosa impossibile con la
    // chiamata bloccante unica usata in precedenza.
    while (1) {
        err = esp_https_ota_perform(handle);
        if (err != ESP_ERR_HTTPS_OTA_IN_PROGRESS) {
            break;
        }
        progress_set_bytes(esp_https_ota_get_image_len_read(handle), total);
    }

    if (err != ESP_OK || !esp_https_ota_is_complete_data_received(handle)) {
        ESP_LOGE(TAG, "Aggiornamento online fallito: download incompleto (perform=%s)", esp_err_to_name(err));
        esp_https_ota_abort(handle);
        FAIL("Aggiornamento fallito, download interrotto prima della fine");
    }

    esp_err_t finish_err = esp_https_ota_finish(handle);
    if (finish_err != ESP_OK) {
        ESP_LOGE(TAG, "Aggiornamento online fallito: immagine non valida (finish=%s)", esp_err_to_name(finish_err));
        FAIL("Aggiornamento fallito, immagine ricevuta non valida");
    }

    SET_MSG("Aggiornamento scaricato e applicato, riavvio...");
    progress_finish(true, out_msg);
    return true;

#undef FAIL
#undef SET_MSG
}

static void online_update_apply_task(void *arg)
{
    char *url = (char *) arg;
    char msg[96] = {0};
    bool ok = online_update_apply(url, msg, sizeof(msg));
    free(url);
    if (ok) {
        ESP_LOGI(TAG, "Firmware aggiornato online, riavvio in corso");
        vTaskDelay(pdMS_TO_TICKS(500));
        esp_restart();
    }
    vTaskDelete(NULL);
}

void online_update_apply_async(const char *firmware_url)
{
    char *url_copy = strdup(firmware_url ? firmware_url : "");
    // Stack allineato a quello gia' necessario per lo stesso lavoro (TLS +
    // scrittura flash) quando girava dentro il task del server web (vedi
    // web_ui_start(), stack_size 10240 con la stessa motivazione).
    if (xTaskCreate(online_update_apply_task, "ota_apply", 10240, url_copy, tskIDLE_PRIORITY + 5, NULL) != pdPASS) {
        free(url_copy);
        progress_reset();
        progress_finish(false, "Impossibile avviare il task di aggiornamento");
    }
}
