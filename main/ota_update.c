#include "ota_update.h"
#include "fw_archive.h"

#include <stdlib.h>
#include <stdio.h>
#include <string.h>

#include "esp_log.h"
#include "esp_ota_ops.h"
#include "freertos/FreeRTOS.h"

static const char *TAG = "ota_update";

#define OTA_BUF_SIZE 4096

// Aggiornamento in corso (vedi ota_update.h). Sezione critica breve: chiamato
// da task diversi (server web, aggiornamento online/automatico, avvio).
static portMUX_TYPE s_busy_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile bool s_busy;

bool ota_update_try_begin(void)
{
    bool ok;
    portENTER_CRITICAL(&s_busy_mux);
    ok = !s_busy;
    if (ok) {
        s_busy = true;
    }
    portEXIT_CRITICAL(&s_busy_mux);
    return ok;
}

void ota_update_end(bool ok)
{
    if (ok) {
        return; // immagine nuova impostata: resta occupato fino al riavvio
    }
    portENTER_CRITICAL(&s_busy_mux);
    s_busy = false;
    portEXIT_CRITICAL(&s_busy_mux);
}

bool ota_update_in_progress(void)
{
    return s_busy;
}

static esp_err_t ota_update_apply_locked(ota_read_fn_t read_cb, void *ctx);

esp_err_t ota_update_apply(ota_read_fn_t read_cb, void *ctx)
{
    if (!ota_update_try_begin()) {
        ESP_LOGW(TAG, "Aggiornamento rifiutato: un altro aggiornamento firmware e' gia' in corso");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = ota_update_apply_locked(read_cb, ctx);
    ota_update_end(err == ESP_OK);
    return err;
}

static esp_err_t ota_update_apply_locked(ota_read_fn_t read_cb, void *ctx)
{
    // Best-effort, non blocca l'aggiornamento se la SD non e' disponibile
    // (vedi fw_archive.h) - salva il firmware ATTUALE prima che questa
    // funzione lo sovrascriva, cosi' resta un ripristino manuale possibile
    // anche a distanza di piu' aggiornamenti.
    fw_archive_save_current();

    const esp_partition_t *target = esp_ota_get_next_update_partition(NULL);
    if (!target) {
        ESP_LOGE(TAG, "Nessuna partizione OTA disponibile");
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Scrittura nuova immagine su partizione '%s' (offset 0x%lx, size 0x%lx)",
             target->label, (unsigned long) target->address, (unsigned long) target->size);

    // Nuovo aggiornamento mentre quello attuale e' ancora in prova (primi 5
    // minuti): esp_ota_begin() lo rifiuterebbe (ESP_ERR_OTA_ROLLBACK_INVALID_STATE)
    // e l'aggiornamento falliva con un generico errore 500 (collaudo del
    // 06/10/2026). La versione attuale funziona (sta servendo la richiesta):
    // la si conferma, e se la nuova fallisce si torna a questa.
    ota_update_mark_valid();

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

static volatile bool s_pending; // firmware nuovo in prova (vedi ota_update_start_confirm)

void ota_update_mark_valid(void)
{
    s_pending = false;
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) == ESP_OK && state == ESP_OTA_IMG_PENDING_VERIFY) {
        esp_ota_mark_app_valid_cancel_rollback();
        ESP_LOGI(TAG, "Immagine corrente confermata valida (rollback automatico disattivato per questo avvio)");
    }
}

// ---------------------------------------------------------------------------
// Conferma ritardata del firmware nuovo. Prima veniva confermato appena
// partito il pannello: un firmware che andava in crash dopo (come la
// 1.19.82 di prova il 02/10/2026) restava installato in un ciclo di crash e
// e' servito il cavo USB. Ora resta "in prova": se si riavvia prima della
// conferma per un crash o un blocco, il bootloader torna da solo alla
// versione precedente.
// ---------------------------------------------------------------------------

#include "status.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define CONFIRM_STABLE_US   (5LL * 60 * 1000000)   // 5 minuti senza crash e caster collegato...
#define CONFIRM_ANYWAY_US   (15LL * 60 * 1000000)  // ...o 15 minuti senza crash in ogni caso

bool ota_update_is_pending(void)
{
    return s_pending;
}

// Chiamata ogni 5 s dal task di sorveglianza (sys_stats.c): nessun task
// proprio, la memoria libera e' poca.
void ota_update_confirm_tick(void)
{
    if (s_pending) {
        int64_t up = esp_timer_get_time();
        ntrip_conn_status_t nt = status_ntrip_get();
        if ((up >= CONFIRM_STABLE_US && nt.connected) || up >= CONFIRM_ANYWAY_US) {
            ESP_LOGI(TAG, "Firmware nuovo stabile da %lld min%s: confermato",
                     (long long) (up / 60000000), nt.connected ? " con il caster collegato" : "");
            ota_update_mark_valid();
        }
    }
}

void ota_update_start_confirm(void)
{
    const esp_partition_t *running = esp_ota_get_running_partition();
    esp_ota_img_states_t state;
    if (esp_ota_get_state_partition(running, &state) != ESP_OK || state != ESP_OTA_IMG_PENDING_VERIFY) {
        return; // gia' confermato (avvio normale)
    }
    s_pending = true;
    ESP_LOGW(TAG, "Firmware nuovo in prova: viene confermato dopo 5 minuti senza problemi. "
                  "Se si riavvia prima per un errore, torna da solo alla versione precedente");
}

// Al primo avvio dopo un ritorno automatico alla versione precedente, lo
// scrive nel log e lo tiene per il pannello. La partizione annullata resta
// segnata come tale fino al prossimo aggiornamento: si confronta con quanto
// gia' segnalato (in NVS) per non ripeterlo a ogni avvio.
static char s_rollback_note[160];

const char *ota_update_rollback_note(void)
{
    return s_rollback_note;
}

void ota_update_check_rollback(void)
{
    const esp_partition_t *bad = esp_ota_get_last_invalid_partition();
    if (!bad) {
        return;
    }
    esp_app_desc_t desc;
    if (esp_ota_get_partition_description(bad, &desc) != ESP_OK) {
        return;
    }
    char key[64];
    snprintf(key, sizeof(key), "%s %.12s %.8s", bad->label, desc.date, desc.time); // data e ora: la versione interna puo' coincidere tra due build
    nvs_handle_t h;
    if (nvs_open("otaroll", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    char seen[64] = "";
    size_t len = sizeof(seen);
    nvs_get_str(h, "seen", seen, &len);
    if (strcmp(seen, key) != 0) {
        snprintf(s_rollback_note, sizeof(s_rollback_note),
                 "l'ultimo aggiornamento (compilato il %s) si e' riavviato prima della conferma: tornato a questa versione",
                 desc.date);
        ESP_LOGE(TAG, "Aggiornamento annullato: %s", s_rollback_note);
        nvs_set_str(h, "seen", key);
        nvs_commit(h);
    }
    nvs_close(h);
}
