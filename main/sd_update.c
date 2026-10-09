#include "sd_update.h"
#include "ota_update.h"
#include "version.h"
#include "sd_mutex.h"
#include "sys_stats.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "esp_log.h"
#include "esp_system.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "cJSON.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "sd_update";

#define MOUNT_POINT "/sdcard"
#define SD_UPDATE_TIMEOUT_MS 15000

// Esito dell'ultimo controllo (boot automatico o pulsante manuale), letto
// dalla UI web (scheda Stato) - protetto da mutex per via del campo
// stringa, non scrivibile/leggibile in modo atomico come un booleano.
static SemaphoreHandle_t s_status_mutex;
static sd_update_status_t s_status;
static bool s_last_card_present;     // scritti direttamente al mount, senza bisogno del mutex (letti solo dallo stesso task)
static uint64_t s_last_total_bytes;
static uint64_t s_last_used_bytes;

static void status_mutex_init(void)
{
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
    }
}

static void set_status(bool card_present, const char *msg, uint64_t total_bytes, uint64_t used_bytes)
{
    status_mutex_init();
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_status.checked = true;
    s_status.card_present = card_present;
    s_status.total_bytes = total_bytes;
    s_status.used_bytes = used_bytes;
    if (msg) {
        strncpy(s_status.message, msg, sizeof(s_status.message) - 1);
        s_status.message[sizeof(s_status.message) - 1] = '\0';
    }
    xSemaphoreGive(s_status_mutex);
}

sd_update_status_t sd_update_get_status(void)
{
    if (!s_status_mutex) {
        return (sd_update_status_t){0};
    }
    sd_update_status_t copy;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    copy = s_status;
    xSemaphoreGive(s_status_mutex);
    return copy;
}

static int ota_read_from_file(void *ctx_ptr, uint8_t *buf, size_t max_len)
{
    FILE *f = (FILE *) ctx_ptr;
    size_t n = fread(buf, 1, max_len, f);
    if (n == 0 && ferror(f)) {
        return -1;
    }
    return (int) n;
}

static bool sd_update_check_and_apply_impl(char *out_msg, size_t out_msg_size)
{
#define SET_MSG(...) do { if (out_msg) snprintf(out_msg, out_msg_size, __VA_ARGS__); } while (0)

    // Bus SPI dedicato (SPI3_HOST), distinto da quello eventualmente usato
    // dall'Ethernet W5500 (SPI2_HOST, vedi eth_link.c) - pin fisici
    // comunque diversi, ma meglio non condividere anche l'istanza host.
    // Preso PRIMA di toccare l'hardware SD, rilasciato su OGNI percorso di
    // uscita di questa funzione (vedi sd_mutex.h) - impedisce a un altro
    // modulo (diag_log, fw_archive, ppp_log) di montare la stessa scheda
    // mentre questa funzione la sta gia' usando.
    sd_mutex_take();

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;

    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_BASEESP32_SD_MOSI_PIN,
        .miso_io_num = CONFIG_BASEESP32_SD_MISO_PIN,
        .sclk_io_num = CONFIG_BASEESP32_SD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    ESP_LOGI(TAG, "Init bus SPI (MISO=%d MOSI=%d SCLK=%d CS=%d)...",
             CONFIG_BASEESP32_SD_MISO_PIN, CONFIG_BASEESP32_SD_MOSI_PIN,
             CONFIG_BASEESP32_SD_SCLK_PIN, CONFIG_BASEESP32_SD_CS_PIN);
    esp_err_t err = spi_bus_initialize((spi_host_device_t) host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Init bus SPI per SD fallita: %s", esp_err_to_name(err));
        SET_MSG("Bus SPI verso la scheda SD non inizializzabile");
        sd_mutex_give();
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = CONFIG_BASEESP32_SD_CS_PIN;
    slot_cfg.host_id = (spi_host_device_t) host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
    };
    ESP_LOGI(TAG, "Mount scheda SD in corso...");
    sdmmc_card_t *card = NULL;
    err = sd_mount_retry(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scheda SD non montata: %s (assente, non inserita, o non formattata FAT32?)", esp_err_to_name(err));
        SET_MSG("Nessuna scheda SD rilevata (verifica inserimento e formato FAT32)");
        s_last_card_present = false;
        s_last_total_bytes = 0;
        s_last_used_bytes = 0;
        spi_bus_free((spi_host_device_t) host.slot);
        sd_mutex_give();
        return false;
    }
    ESP_LOGI(TAG, "Scheda SD montata correttamente");
    s_last_card_present = true;

    uint64_t total_bytes = 0, free_bytes = 0;
    if (esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes) == ESP_OK) {
        s_last_total_bytes = total_bytes;
        s_last_used_bytes = total_bytes - free_bytes;
        ESP_LOGI(TAG, "Spazio SD: %llu MB totali, %llu MB usati",
                 (unsigned long long) (total_bytes / (1024 * 1024)),
                 (unsigned long long) (s_last_used_bytes / (1024 * 1024)));
    } else {
        ESP_LOGW(TAG, "Impossibile leggere la capacita' della scheda SD");
        s_last_total_bytes = 0;
        s_last_used_bytes = 0;
    }

    bool applied = false;
    cJSON *root = NULL;

    FILE *jf = fopen(MOUNT_POINT "/firmware.json", "r");
    if (!jf) {
        SET_MSG("nessun aggiornamento da installare");
        goto cleanup;
    }
    char json_buf[128] = {0};
    fread(json_buf, 1, sizeof(json_buf) - 1, jf);
    fclose(jf);

    root = cJSON_Parse(json_buf);
    if (!root) {
        SET_MSG("firmware.json non valido");
        goto cleanup;
    }
    cJSON *ver_item = cJSON_GetObjectItemCaseSensitive(root, "version");
    if (!ver_item || !cJSON_IsString(ver_item)) {
        SET_MSG("firmware.json senza campo \"version\"");
        goto cleanup;
    }

    char sd_version[32];
    strncpy(sd_version, ver_item->valuestring, sizeof(sd_version) - 1);
    sd_version[sizeof(sd_version) - 1] = '\0';

    if (ota_update_semver_compare(sd_version, FIRMWARE_VERSION) <= 0) {
        SET_MSG("Versione sulla SD (%s) non piu' recente dell'attuale (%s)", sd_version, FIRMWARE_VERSION);
        goto cleanup;
    }

    FILE *bf = fopen(MOUNT_POINT "/firmware.bin", "rb");
    if (!bf) {
        SET_MSG("firmware.bin non trovato sulla scheda SD");
        goto cleanup;
    }

    ESP_LOGI(TAG, "Versione %s trovata su SD (attuale %s), applico...", sd_version, FIRMWARE_VERSION);
    err = ota_update_apply(ota_read_from_file, bf);
    fclose(bf);

    if (err != ESP_OK) {
        SET_MSG("Aggiornamento da SD fallito, firmware attuale non modificato");
        goto cleanup;
    }

    rename(MOUNT_POINT "/firmware.bin", MOUNT_POINT "/firmware.bin.applied");
    SET_MSG("Aggiornato a versione %s, riavvio...", sd_version);
    applied = true;

cleanup:
    if (root) {
        cJSON_Delete(root);
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    spi_bus_free((spi_host_device_t) host.slot);
    sd_mutex_give();
    return applied;

#undef SET_MSG
}

// Contesto condiviso tra sd_update_check_and_apply() e il task che esegue
// il lavoro vero: allocato sull'heap (non sullo stack del chiamante)
// perche', se scatta il timeout, il task puo' finire piu' tardi e deve
// comunque trovare un contesto valido a cui scrivere.
typedef struct {
    char out_msg[96];
    bool result;
    bool finished;  // il task ha finito e il chiamante lo sta aspettando
    bool abandoned; // il chiamante e' andato in timeout: il task si arrangia da solo
    SemaphoreHandle_t done;
} sd_task_ctx_t;

// Protegge finished/abandoned: chiamante e task decidono insieme chi libera
// il contesto e chi riavvia, anche se il timeout scade proprio mentre il
// task finisce.
static portMUX_TYPE s_ctx_mux = portMUX_INITIALIZER_UNLOCKED;

static void sd_update_task(void *arg)
{
    sd_task_ctx_t *ctx = (sd_task_ctx_t *) arg;
    bool result = sd_update_check_and_apply_impl(ctx->out_msg, sizeof(ctx->out_msg));
    taskENTER_CRITICAL(&s_ctx_mux);
    ctx->result = result;
    bool orphan = ctx->abandoned;
    if (!orphan) {
        ctx->finished = true;
    }
    taskEXIT_CRITICAL(&s_ctx_mux);
    if (!orphan) {
        xSemaphoreGive(ctx->done);
        vTaskDelete(NULL);
        return;
    }
    // Il chiamante ha gia' rinunciato. Se l'aggiornamento e' riuscito la
    // partizione di avvio e' gia' quella nuova: si riavvia subito, altrimenti
    // il firmware nuovo partirebbe a sorpresa al primo riavvio, magari giorni dopo.
    set_status(s_last_card_present, ctx->out_msg, s_last_total_bytes, s_last_used_bytes);
    if (result) {
        ESP_LOGW(TAG, "Aggiornamento da microSD finito dopo il timeout (%s): riavvio", ctx->out_msg);
        sys_stats_note_restart_reason("aggiornamento firmware da microSD (finito dopo l'attesa)");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    ESP_LOGW(TAG, "Controllo della microSD finito dopo il timeout: %s", ctx->out_msg);
    vSemaphoreDelete(ctx->done);
    free(ctx);
    vTaskDelete(NULL);
}

// La UI web (e chi la usa) non deve mai restare bloccata a tempo
// indeterminato per un problema hardware della SD (scheda difettosa,
// contatti sporchi, ecc.) - il lavoro vero gira in un task a parte con un
// timeout massimo. Se scade, il task orfano continua in background e libera
// lui il contesto; se poi l'aggiornamento riesce riavvia lui la base (prima
// la partizione nuova restava impostata e partiva al primo riavvio qualunque).
bool sd_update_check_and_apply(char *out_msg, size_t out_msg_size)
{
    return sd_update_check_and_apply_timeout(out_msg, out_msg_size, SD_UPDATE_TIMEOUT_MS);
}

bool sd_update_check_and_apply_timeout(char *out_msg, size_t out_msg_size, uint32_t timeout_ms)
{
    sd_task_ctx_t *ctx = calloc(1, sizeof(sd_task_ctx_t));
    if (!ctx) {
        if (out_msg) {
            snprintf(out_msg, out_msg_size, "Memoria insufficiente");
        }
        return false;
    }
    ctx->done = xSemaphoreCreateBinary();

    if (xTaskCreate(sd_update_task, "sd_update", 4096, ctx, 5, NULL) != pdPASS) {
        vSemaphoreDelete(ctx->done);
        free(ctx);
        if (out_msg) {
            snprintf(out_msg, out_msg_size, "Impossibile avviare il controllo della SD");
        }
        return false;
    }

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        taskENTER_CRITICAL(&s_ctx_mux);
        bool finished = ctx->finished;
        if (!finished) {
            ctx->abandoned = true;
        }
        taskEXIT_CRITICAL(&s_ctx_mux);
        if (!finished) {
            ESP_LOGE(TAG, "Timeout (%lu ms) durante il controllo della scheda SD - il tentativo continua in background "
                          "(se aggiorna il firmware, la base si riavvia da sola)", (unsigned long) timeout_ms);
            if (out_msg) {
                snprintf(out_msg, out_msg_size, "La scheda SD non risponde (timeout) - verifica contatti/formato e riprova");
            }
            set_status(false, "La scheda SD non risponde (timeout)", 0, 0);
            return false; // ctx e ctx->done li libera il task orfano, vedi sd_update_task
        }
        // Finito proprio allo scadere: il segnale arriva subito.
        xSemaphoreTake(ctx->done, portMAX_DELAY);
    }

    bool result = ctx->result;
    if (out_msg) {
        strncpy(out_msg, ctx->out_msg, out_msg_size - 1);
        out_msg[out_msg_size - 1] = '\0';
    }
    set_status(s_last_card_present, ctx->out_msg, s_last_total_bytes, s_last_used_bytes);
    vSemaphoreDelete(ctx->done);
    free(ctx);
    return result;
}

bool sd_format_card(char *out_msg, size_t out_msg_size)
{
    sd_mutex_take();

    sdmmc_host_t host = SDSPI_HOST_DEFAULT();
    host.slot = SPI3_HOST;
    spi_bus_config_t bus_cfg = {
        .mosi_io_num = CONFIG_BASEESP32_SD_MOSI_PIN,
        .miso_io_num = CONFIG_BASEESP32_SD_MISO_PIN,
        .sclk_io_num = CONFIG_BASEESP32_SD_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = 4000,
    };
    if (spi_bus_initialize((spi_host_device_t) host.slot, &bus_cfg, SDSPI_DEFAULT_DMA) != ESP_OK) {
        snprintf(out_msg, out_msg_size, "Bus SPI verso la scheda SD non inizializzabile");
        sd_mutex_give();
        return false;
    }
    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = CONFIG_BASEESP32_SD_CS_PIN;
    slot_cfg.host_id = (spi_host_device_t) host.slot;

    // Cluster da 32 KB: FAT piu' piccola (formattazione piu' veloce su schede
    // grandi) e valore standard per FAT32.
    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 2,
        .allocation_unit_size = 32 * 1024,
    };
    sdmmc_card_t *card = NULL;
    ESP_LOGW(TAG, "Formattazione microSD in FAT32 richiesta dal pannello");
    esp_err_t err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
    if (err == ESP_OK) {
        // Filesystem gia' leggibile: formattazione esplicita (cancella tutto).
        err = esp_vfs_fat_sdcard_format_cfg(MOUNT_POINT, card, &mount_cfg);
    } else if (err == ESP_FAIL) {
        // La scheda risponde ma il formato non e' leggibile (exFAT, partizioni
        // di un Raspberry...): la formatta il montaggio stesso, una volta sola.
        mount_cfg.format_if_mount_failed = true;
        err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
    }

    bool ok = (err == ESP_OK);
    if (ok) {
        uint64_t total_bytes = 0, free_bytes = 0;
        esp_vfs_fat_info(MOUNT_POINT, &total_bytes, &free_bytes);
        snprintf(out_msg, out_msg_size, "microSD formattata in FAT32: %llu MB disponibili",
                 (unsigned long long) (total_bytes / (1024 * 1024)));
        ESP_LOGI(TAG, "%s", out_msg);
        s_last_card_present = true;
        s_last_total_bytes = total_bytes;
        s_last_used_bytes = total_bytes - free_bytes;
        set_status(true, "microSD formattata in FAT32", total_bytes, total_bytes - free_bytes);
        esp_vfs_fat_sdcard_unmount(MOUNT_POINT, card);
    } else if (err == ESP_ERR_TIMEOUT) {
        snprintf(out_msg, out_msg_size, "Nessuna microSD risponde: controlla che sia inserita bene");
    } else {
        snprintf(out_msg, out_msg_size, "Formattazione non riuscita (%s)", esp_err_to_name(err));
        ESP_LOGE(TAG, "%s", out_msg);
    }
    spi_bus_free((spi_host_device_t) host.slot);
    sd_mutex_give();
    return ok;
}
