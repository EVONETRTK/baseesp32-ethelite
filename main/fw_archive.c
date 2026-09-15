#include "fw_archive.h"
#include "ota_update.h"
#include "version.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_ota_ops.h"
#include "esp_partition.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

static const char *TAG = "fw_archive";

#define MOUNT_POINT  "/sdcard"
#define ARCHIVE_DIR  MOUNT_POINT "/firmware"
#define KEEP_VERSIONS 2

static sdmmc_card_t *s_card;
static bool s_sd_mounted;

// Stesso schema di montaggio di sd_update.c/ppp_log.c (bus SPI dedicato,
// montato solo per la durata dell'operazione) - duplicato qui invece di
// condiviso per tenere ogni modulo autonomo, come gia' fatto altrove in
// questo progetto.
static bool mount_sd(void)
{
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
    esp_err_t err = spi_bus_initialize((spi_host_device_t) host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD non disponibile (bus SPI): %s", esp_err_to_name(err));
        return false;
    }

    sdspi_device_config_t slot_cfg = SDSPI_DEVICE_CONFIG_DEFAULT();
    slot_cfg.gpio_cs = CONFIG_BASEESP32_SD_CS_PIN;
    slot_cfg.host_id = (spi_host_device_t) host.slot;

    esp_vfs_fat_sdmmc_mount_config_t mount_cfg = {
        .format_if_mount_failed = false,
        .max_files = 4,
    };
    err = esp_vfs_fat_sdspi_mount(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD non montata: %s", esp_err_to_name(err));
        spi_bus_free((spi_host_device_t) host.slot);
        return false;
    }
    s_sd_mounted = true;
    return true;
}

static void unmount_sd(void)
{
    if (!s_sd_mounted) {
        return;
    }
    esp_vfs_fat_sdcard_unmount(MOUNT_POINT, s_card);
    spi_bus_free(SPI3_HOST);
    s_sd_mounted = false;
    s_card = NULL;
}

// "v1.15.0.bin" -> "1.15.0". Ritorna NULL se il nome non ha la forma attesa.
static const char *version_from_filename(const char *filename, char *out, size_t out_size)
{
    if (filename[0] != 'v') {
        return NULL;
    }
    size_t len = strlen(filename);
    if (len < 5 || strcmp(filename + len - 4, ".bin") != 0) {
        return NULL;
    }
    size_t ver_len = len - 4 - 1; // meno "v" iniziale e ".bin" finale
    if (ver_len >= out_size) {
        ver_len = out_size - 1;
    }
    memcpy(out, filename + 1, ver_len);
    out[ver_len] = '\0';
    return out;
}

// Elenca i nomi file dell'archivio (senza ordinarli) - usata sia da
// fw_archive_list() sia dalla potatura in fw_archive_save_current().
static size_t list_raw(fw_archive_entry_t *out, size_t max_count)
{
    DIR *d = opendir(ARCHIVE_DIR);
    if (!d) {
        return 0;
    }
    size_t n = 0;
    struct dirent *de;
    while (n < max_count && (de = readdir(d)) != NULL) {
        char ver[16];
        if (!version_from_filename(de->d_name, ver, sizeof(ver))) {
            continue; // non e' un file d'archivio riconoscibile, ignoralo
        }
        strncpy(out[n].filename, de->d_name, sizeof(out[n].filename) - 1);
        out[n].filename[sizeof(out[n].filename) - 1] = '\0';
        n++;
    }
    closedir(d);
    return n;
}

static int compare_entries_newest_first(const void *a, const void *b)
{
    const fw_archive_entry_t *ea = (const fw_archive_entry_t *) a;
    const fw_archive_entry_t *eb = (const fw_archive_entry_t *) b;
    char va[16], vb[16];
    version_from_filename(ea->filename, va, sizeof(va));
    version_from_filename(eb->filename, vb, sizeof(vb));
    return ota_update_semver_compare(vb, va); // invertito: piu' recente prima
}

void fw_archive_save_current(void)
{
    if (!mount_sd()) {
        return; // best-effort, non blocca l'aggiornamento in corso
    }
    mkdir(ARCHIVE_DIR, 0755); // ESP_OK anche se esiste gia' (errno EEXIST ignorato)

    const esp_partition_t *running = esp_ota_get_running_partition();
    if (!running) {
        ESP_LOGW(TAG, "Partizione in esecuzione non trovata, archiviazione saltata");
        unmount_sd();
        return;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/v%s.bin", ARCHIVE_DIR, FIRMWARE_VERSION);

    // Se questa versione e' gia' archiviata (chiamata ripetuta all'avvio,
    // vedi main.c - non solo prima di un aggiornamento) non serve
    // riscrivere l'intera partizione ad ogni riavvio: inutile usura della
    // SD nel tempo su un dispositivo che puo' restare acceso mesi in campo.
    FILE *existing = fopen(path, "rb");
    if (existing) {
        fclose(existing);
        unmount_sd();
        return;
    }

    FILE *f = fopen(path, "wb");
    if (!f) {
        ESP_LOGW(TAG, "Impossibile creare %s, archiviazione saltata", path);
        unmount_sd();
        return;
    }

    // Copia l'intera dimensione della partizione (non solo l'immagine
    // effettiva, che richiederebbe analizzare l'header per la dimensione
    // esatta): piu' semplice e sicuro, lo spazio su SD e' abbondante.
    uint8_t buf[4096];
    bool ok = true;
    for (size_t off = 0; off < running->size; off += sizeof(buf)) {
        size_t chunk = running->size - off < sizeof(buf) ? running->size - off : sizeof(buf);
        if (esp_partition_read(running, off, buf, chunk) != ESP_OK) {
            ok = false;
            break;
        }
        if (fwrite(buf, 1, chunk, f) != chunk) {
            ok = false;
            break;
        }
    }
    fclose(f);

    if (!ok) {
        ESP_LOGW(TAG, "Copia della partizione fallita, rimuovo il file incompleto");
        unlink(path);
        unmount_sd();
        return;
    }
    ESP_LOGI(TAG, "Firmware attuale (v%s) archiviato su SD: %s", FIRMWARE_VERSION, path);

    // Potatura: tieni solo le KEEP_VERSIONS piu' recenti.
    fw_archive_entry_t entries[16];
    size_t n = list_raw(entries, sizeof(entries) / sizeof(entries[0]));
    qsort(entries, n, sizeof(entries[0]), compare_entries_newest_first);
    for (size_t i = KEEP_VERSIONS; i < n; i++) {
        char old_path[64];
        snprintf(old_path, sizeof(old_path), "%s/%s", ARCHIVE_DIR, entries[i].filename);
        if (unlink(old_path) == 0) {
            ESP_LOGI(TAG, "Rimossa versione piu' vecchia dall'archivio: %s", entries[i].filename);
        }
    }

    unmount_sd();
}

size_t fw_archive_list(fw_archive_entry_t *out, size_t max_count)
{
    if (!mount_sd()) {
        return 0;
    }
    size_t n = list_raw(out, max_count);
    qsort(out, n, sizeof(out[0]), compare_entries_newest_first);
    unmount_sd();
    return n;
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

bool fw_archive_apply(const char *filename, char *out_msg, size_t out_msg_size)
{
#define SET_MSG(...) do { if (out_msg) snprintf(out_msg, out_msg_size, __VA_ARGS__); } while (0)

    // Un nome file con "/" o ".." permetterebbe di uscire dalla cartella
    // archivio - non plausibile dalla UI (che propone solo nomi gia'
    // elencati da fw_archive_list), ma controllato comunque per sicurezza
    // dato che il valore arriva da una richiesta HTTP.
    if (!filename[0] || strstr(filename, "..") || strchr(filename, '/')) {
        SET_MSG("Nome file non valido");
        return false;
    }

    if (!mount_sd()) {
        SET_MSG("Scheda SD non disponibile");
        return false;
    }

    char path[64];
    snprintf(path, sizeof(path), "%s/%s", ARCHIVE_DIR, filename);
    FILE *f = fopen(path, "rb");
    if (!f) {
        SET_MSG("File non trovato nell'archivio: %s", filename);
        unmount_sd();
        return false;
    }

    ESP_LOGW(TAG, "Ripristino manuale (ignora versione) da %s", path);
    esp_err_t err = ota_update_apply(ota_read_from_file, f);
    fclose(f);
    unmount_sd();

    if (err != ESP_OK) {
        SET_MSG("Ripristino fallito, firmware attuale non modificato");
        return false;
    }
    SET_MSG("Ripristinata versione da %s, riavvio...", filename);
    return true;

#undef SET_MSG
}
