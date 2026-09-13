#include "sd_update.h"
#include "ota_update.h"
#include "version.h"

#include "sdkconfig.h"

#include <stdio.h>
#include <string.h>

#include <stdlib.h>

#include "esp_log.h"
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
    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "Scheda SD non montata: %s (assente, non inserita, o non formattata FAT32?)", esp_err_to_name(err));
        SET_MSG("Nessuna scheda SD rilevata (verifica inserimento e formato FAT32)");
        spi_bus_free((spi_host_device_t) host.slot);
        return false;
    }
    ESP_LOGI(TAG, "Scheda SD montata correttamente");

    bool applied = false;
    cJSON *root = NULL;

    FILE *jf = fopen(MOUNT_POINT "/firmware.json", "r");
    if (!jf) {
        SET_MSG("Nessun firmware.json trovato sulla scheda SD");
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
    SemaphoreHandle_t done;
} sd_task_ctx_t;

static void sd_update_task(void *arg)
{
    sd_task_ctx_t *ctx = (sd_task_ctx_t *) arg;
    ctx->result = sd_update_check_and_apply_impl(ctx->out_msg, sizeof(ctx->out_msg));
    xSemaphoreGive(ctx->done);
    vTaskDelete(NULL);
}

// La UI web (e chi la usa) non deve mai restare bloccata a tempo
// indeterminato per un problema hardware della SD (scheda difettosa,
// contatti sporchi, ecc.) - il lavoro vero gira in un task a parte con un
// timeout massimo. Se scade, il task orfano continua comunque in
// background (il suo contesto viene volutamente non liberato in quel
// caso, per evitare un crash se scrivesse su memoria gia' rilasciata) -
// accettabile per un'azione manuale/occasionale come questa.
bool sd_update_check_and_apply(char *out_msg, size_t out_msg_size)
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

    if (xSemaphoreTake(ctx->done, pdMS_TO_TICKS(SD_UPDATE_TIMEOUT_MS)) != pdTRUE) {
        ESP_LOGE(TAG, "Timeout (%d ms) durante il controllo della scheda SD - il tentativo continua in background",
                 SD_UPDATE_TIMEOUT_MS);
        if (out_msg) {
            snprintf(out_msg, out_msg_size, "La scheda SD non risponde (timeout) - verifica contatti/formato e riprova");
        }
        return false; // ctx e ctx->done restano vivi per il task orfano, vedi commento sopra
    }

    bool result = ctx->result;
    if (out_msg) {
        strncpy(out_msg, ctx->out_msg, out_msg_size - 1);
        out_msg[out_msg_size - 1] = '\0';
    }
    vSemaphoreDelete(ctx->done);
    free(ctx);
    return result;
}
