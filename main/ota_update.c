#include "ota_update.h"

#include <stdlib.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_ota_ops.h"

static const char *TAG = "ota_update";

#define OTA_BUF_SIZE 4096

esp_err_t ota_update_apply(ota_read_fn_t read_cb, void *ctx)
{
    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "Nessuna partizione OTA disponibile");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Scrittura nuova immagine su partizione '%s' (offset 0x%lx, size 0x%lx)",
             target->label, (unsigned long) target->address, (unsigned long) target->size);

    esp_ota_handle_t handle;
    esp_err_t err = esp_ota_begin(target, OTA_SIZE_UNKNOWN, &handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_begin fallito: %s", esp_err_to_name(err));
        return err;
    }

    uint8_t *buf = malloc(OTA_BUF_SIZE);
    if (!buf) {
        esp_ota_abort(handle);
        return ESP_ERR_NO_MEM;
    }

    size_t total = 0;
    while (1) {
        int n = read_cb(ctx, buf, OTA_BUF_SIZE);
        if (n < 0) {
            ESP_LOGE(TAG, "Lettura sorgente immagine fallita dopo %u byte", (unsigned) total);
            free(buf);
            esp_ota_abort(handle);
            return ESP_FAIL;
        }
        if (n == 0) {
            break; // fine immagine
        }
        err = esp_ota_write(handle, buf, (size_t) n);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "esp_ota_write fallito dopo %u byte: %s", (unsigned) total, esp_err_to_name(err));
            free(buf);
            esp_ota_abort(handle);
            return err;
        }
        total += (size_t) n;
    }
    free(buf);

    if (total == 0) {
        ESP_LOGE(TAG, "Immagine vuota, aggiornamento annullato");
        esp_ota_abort(handle);
        return ESP_FAIL;
    }

    err = esp_ota_end(handle);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_end fallito (immagine non valida?): %s", esp_err_to_name(err));
        return err;
    }

    err = esp_ota_set_boot_partition(target);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "esp_ota_set_boot_partition fallito: %s", esp_err_to_name(err));
        return err;
    }

    ESP_LOGI(TAG, "Immagine (%u byte) scritta e impostata come avvio successivo", (unsigned) total);
    return ESP_OK;
}

int ota_update_semver_compare(const char *a, const char *b)
{
    int a_maj = 0, a_min = 0, a_pat = 0;
    int b_maj = 0, b_min = 0, b_pat = 0;
    sscanf(a, "%d.%d.%d", &a_maj, &a_min, &a_pat);
    sscanf(b, "%d.%d.%d", &b_maj, &b_min, &b_pat);
    if (a_maj != b_maj) {
        return a_maj - b_maj;
    }
    if (a_min != b_min) {
        return a_min - b_min;
    }
    return a_pat - b_pat;
}

void ota_update_mark_valid(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Immagine corrente confermata valida (rollback automatico disattivato per questo avvio)");
    }
}
