#include "config_backup.h"
#include "settings.h"
#include "sd_mutex.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_attr.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "sys_stats.h"
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
#define GOOD_FILE    BACKUP_DIR "/settings_good.bin"

// Protezione dai riavvii a catena (vedi config_backup.h).
#define GUARD_MAGIC        0x47524444u
#define GUARD_MAX_FAULTS   3
#define GUARD_STABLE_US    (10LL * 60 * 1000000)       // avvio riuscito dopo 10 min
#define GOOD_AFTER_US      (2LL * 60 * 60 * 1000000)   // configurazione buona dopo 2 ore
typedef struct {
    uint32_t magic;
    uint32_t faults;   // avvii di fila finiti con un guasto prima di GUARD_STABLE_US
} guard_rtc_t;
static RTC_NOINIT_ATTR guard_rtc_t s_guard;
static int64_t s_good_due_us = GOOD_AFTER_US; // 0 = gia' scritta
static char s_guard_note[96];
#define BLOB_MAX     2816 // >= sizeof(stored_cfg_t) (~2,4 KB con la VPN, 1.19.113), con margine per i campi futuri

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
    // Configurazione cambiata: diventa "buona" solo dopo altre 2 ore senza guasti.
    s_good_due_us = esp_timer_get_time() + GOOD_AFTER_US;
}

static void write_file(const char *path, const char *tmp_path)
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
    FILE *f = fopen(tmp_path, "wb");
    if (f) {
        ok = fwrite(s_blob, 1, len, f) == len;
        ok = (fclose(f) == 0) && ok;
    }
    if (ok) {
        unlink(path);
        ok = rename(tmp_path, path) == 0;
    }
    unmount_sd();
    if (ok) {
        ESP_LOGI(TAG, "Copia della configurazione salvata sulla microSD: %s (%u byte)", path, (unsigned) len);
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
        write_file(BACKUP_FILE, BACKUP_TMP);
    }
    int64_t up = esp_timer_get_time();
    if (up >= GUARD_STABLE_US && s_guard.faults != 0) {
        ESP_LOGI(TAG, "Avvio stabile: azzerato il conteggio dei riavvii per guasto (era %u)",
                 (unsigned) s_guard.faults);
        s_guard.faults = 0;
    }
    if (s_good_due_us > 0 && up >= s_good_due_us) {
        s_good_due_us = 0;
        write_file(GOOD_FILE, BACKUP_DIR "/settings_good.tmp");
    }
}

void config_backup_crash_guard(void)
{
    if (s_guard.magic != GUARD_MAGIC || esp_reset_reason() == ESP_RST_POWERON) {
        s_guard.magic = GUARD_MAGIC; // memoria RTC non valida dopo l'accensione
        s_guard.faults = 0;
    }
    if (!sys_stats_boot_was_fault()) {
        s_guard.faults = 0;
        return;
    }
    s_guard.faults++;
    ESP_LOGW(TAG, "Avvio dopo un guasto: %u di fila prima di %d minuti di funzionamento",
             (unsigned) s_guard.faults, (int) (GUARD_STABLE_US / 60000000));
    if (s_guard.faults < GUARD_MAX_FAULTS) {
        return;
    }
    s_guard.faults = 0;
    if (!mount_sd()) {
        ESP_LOGE(TAG, "Riavvii a catena, ma microSD non disponibile: configurazione buona non ripristinabile");
        return;
    }
    size_t len = 0;
    FILE *f = fopen(GOOD_FILE, "rb");
    if (f) {
        len = fread(s_blob, 1, sizeof(s_blob), f);
        fclose(f);
    }
    unmount_sd();
    if (len == 0) {
        ESP_LOGE(TAG, "Riavvii a catena, ma nessuna configurazione buona sulla microSD");
        return;
    }
    if (settings_import_blob(s_blob, len, false) != ESP_OK) {
        ESP_LOGE(TAG, "Configurazione buona sulla microSD non valida (%u byte)", (unsigned) len);
        return;
    }
    settings_init();
    snprintf(s_guard_note, sizeof(s_guard_note),
             "%d riavvii per guasto di fila: ripristinata l'ultima configurazione buona", GUARD_MAX_FAULTS);
    ESP_LOGW(TAG, "%s", s_guard_note);
}

void config_backup_save_good_now(void)
{
    s_good_due_us = 1;
}

const char *config_backup_guard_note(void)
{
    return s_guard_note;
}

void config_backup_start(void)
{
    s_requested = true; // prima copia al primo giro del log, anche senza salvataggi
}
