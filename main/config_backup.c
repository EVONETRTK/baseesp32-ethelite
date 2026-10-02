#include "config_backup.h"
#include "settings.h"
#include "sd_mutex.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "nvs.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "config_backup";

#define MOUNT_POINT  "/sdcard"
#define BACKUP_DIR   MOUNT_POINT "/config"
#define BACKUP_FILE  BACKUP_DIR "/settings.bin"
#define BACKUP_TMP   BACKUP_DIR "/settings.tmp"
#define BLOB_MAX     2304 // >= sizeof(stored_cfg_t) (2072 byte oggi), con margine per i campi futuri

static volatile bool s_requested;
static sdmmc_card_t *s_card;
static uint8_t s_blob[BLOB_MAX]; // statico: mai sullo stack (vedi settings.c)
_Static_assert(BLOB_MAX >= sizeof(app_settings_t) + 8, "BLOB_MAX troppo piccolo per la configurazione");

// Stesso schema di montaggio degli altri moduli SD (diag_log.c, ppp_log.c):
// sd_mutex preso qui e rilasciato in unmount_sd().
static bool mount_sd(void)
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
    if (sd_mount_retry(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card) != ESP_OK) {
        spi_bus_free((spi_host_device_t) host.slot);
        sd_mutex_give();
        return false;
    }
    return true;
}

static void unmount_sd(void)
{
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    spi_bus_free(SPI3_HOST);
    s_card = NULL;
    sd_mutex_give();
}

void config_backup_request(void)
{
    s_requested = true;
}

static void write_backup(void)
{
    size_t len = settings_export_blob(s_blob, sizeof(s_blob));
    if (len == 0) {
        return; // niente in NVS (default di fabbrica): non sovrascrivere una copia buona
    }
    if (!mount_sd()) {
        ESP_LOGW(TAG, "microSD non disponibile, copia della configurazione rimandata al prossimo giro");
        s_requested = true;
        return;
    }
    mkdir(BACKUP_DIR, 0755);
    // Scrittura su file temporaneo e poi rinomina: se manca la corrente a
    // meta', la copia precedente resta integra.
    bool ok = false;
    FILE *f = fopen(BACKUP_TMP, "wb");
    if (f) {
        ok = fwrite(s_blob, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
    }
    if (ok) {
        unlink(BACKUP_FILE);
        ok = rename(BACKUP_TMP, BACKUP_FILE) == 0;
    }
    unmount_sd();
    if (ok) {
        ESP_LOGI(TAG, "Copia della configurazione salvata sulla microSD (%u byte)", (unsigned) len);
    } else {
        ESP_LOGW(TAG, "Scrittura della copia della configurazione fallita");
    }
}

bool config_backup_restore_if_missing(void)
{
    if (settings_loaded_from_nvs()) {
        return false;
    }
    nvs_handle_t h;
    if (nvs_open("cfgbk", NVS_READWRITE, &h) == ESP_OK) {
        uint8_t skip = 0;
        nvs_get_u8(h, "skip", &skip);
        if (skip) {
            nvs_erase_key(h, "skip");
            nvs_commit(h);
            nvs_close(h);
            ESP_LOGW(TAG, "Reset di fabbrica dal pulsante: la copia sulla microSD non viene ripristinata");
            return false;
        }
        nvs_close(h);
    }
    if (!mount_sd()) {
        ESP_LOGW(TAG, "Configurazione assente e microSD non disponibile: restano i valori di fabbrica");
        return false;
    }
    size_t len = 0;
    FILE *f = fopen(BACKUP_FILE, "rb");
    if (f) {
        len = fread(s_blob, 1, sizeof(s_blob), f);
        fclose(f);
    }
    unmount_sd();
    if (len == 0) {
        ESP_LOGW(TAG, "Configurazione assente e nessuna copia sulla microSD: restano i valori di fabbrica");
        return false;
    }
    if (settings_import_blob(s_blob, len, false) != ESP_OK) {
        ESP_LOGE(TAG, "Copia della configurazione sulla microSD non valida (%u byte)", (unsigned) len);
        return false;
    }
    settings_init(); // ricarica, con le stesse migrazioni di un avvio normale
    ESP_LOGW(TAG, "Configurazione ripristinata dalla copia sulla microSD (%s)",
             settings_loaded_from_nvs() ? "riuscito" : "ricaricamento fallito");
    return settings_loaded_from_nvs();
}

// Nessun task proprio (la memoria libera e' poca: un task in piu' da 4 KB
// faceva fallire il montaggio della SD e bloccava il pannello, visto con la
// prima versione). La copia la fa il task del log diagnostico, che monta
// gia' la SD ogni 30 s; se fallisce si riprova al giro dopo.
void config_backup_service(void)
{
    if (s_requested) {
        s_requested = false;
        write_backup();
    }
}

void config_backup_start(void)
{
    s_requested = true; // prima copia al primo giro del log, anche senza salvataggi
}
