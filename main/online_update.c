#include "online_update.h"
#include "version.h"
#include "ota_update.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_http_client.h"
#include "esp_https_ota.h"
#include "esp_crt_bundle.h"
#include "cJSON.h"

static const char *TAG = "online_update";

// Accumula il corpo della risposta HTTP (il manifest JSON, sempre
// piccolo) in un buffer a dimensione fissa fornito dal chiamante.
typedef struct {
    char *buf;
    size_t size;
    size_t used;
} http_download_ctx_t;

static esp_err_t http_event_handler(esp_http_client_event_t *evt)
{
    http_download_ctx_t *ctx = (http_download_ctx_t *) evt->user_data;
    if (evt->event_id == HTTP_EVENT_ON_DATA && ctx && ctx->used + 1 < ctx->size) {
        size_t space = ctx->size - ctx->used - 1;
        size_t n = (size_t) evt->data_len < space ? (size_t) evt->data_len : space;
        memcpy(ctx->buf + ctx->used, evt->data, n);
        ctx->used += n;
        ctx->buf[ctx->used] = '\0';
    }
    return ESP_OK;
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
    http_download_ctx_t ctx = { .buf = manifest, .size = sizeof(manifest), .used = 0 };

    esp_http_client_config_t config = {
        .url = settings.ota_update_url,
        .event_handler = http_event_handler,
        .user_data = &ctx,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    if (err != ESP_OK || status != 200) {
        ESP_LOGW(TAG, "Controllo aggiornamenti fallito: err=%s status=%d", esp_err_to_name(err), status);
        SET_MSG("Impossibile raggiungere l'indirizzo di aggiornamento configurato");
        return false;
    }

    cJSON *root = cJSON_Parse(manifest);
    if (!root) {
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

    esp_http_client_config_t http_config = {
        .url = firmware_url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 30000,
        .keep_alive_enable = true,
    };
    esp_https_ota_config_t ota_config = {
        .http_config = &http_config,
    };

    ESP_LOGI(TAG, "Download aggiornamento online da %s...", firmware_url);
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
