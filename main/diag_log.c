#include "diag_log.h"
#include "log_buffer.h"
#include "sd_mutex.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "diag_log";

#define MOUNT_POINT    "/sdcard"
#define LOG_DIR        MOUNT_POINT "/diag_logs"
#define MARKER_FILE    LOG_DIR "/.next_index"
#define ROTATE_COUNT   5
#define MAX_FILE_BYTES (200 * 1024)
#define FLUSH_INTERVAL_MS (30 * 1000)

static StreamBufferHandle_t s_stream;

static sdmmc_card_t *s_card;
static bool s_sd_mounted;

// Stesso schema di montaggio di ppp_log.c/fw_archive.c - duplicato per
// tenere ogni modulo autonomo.
// sd_mutex_take() e' preso qui (non nel chiamante) e rilasciato in
// unmount_sd() - o subito, se il montaggio stesso fallisce - cosi'
// ogni chiamante di mount_sd() lo ottiene/rilascia automaticamente
// senza doverci pensare. Vedi sd_mutex.h per il motivo (contesa reale
// con altri moduli, confermata su hardware).
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
    esp_err_t err = spi_bus_initialize((spi_host_device_t) host.slot, &bus_cfg, SDSPI_DEFAULT_DMA);
    if (err != ESP_OK) {
        sd_mutex_give();
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
        spi_bus_free((spi_host_device_t) host.slot);
        sd_mutex_give();
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
    sd_mutex_give();
}

static int read_next_index(void)
{
    FILE *f = fopen(MARKER_FILE, "r");
    if (!f) {
        return 0;
    }
    int idx = 0;
    if (fscanf(f, "%d", &idx) != 1 || idx < 0 || idx >= ROTATE_COUNT) {
        idx = 0;
    }
    fclose(f);
    return idx;
}

static void write_next_index(int idx)
{
    FILE *f = fopen(MARKER_FILE, "w");
    if (!f) {
        return;
    }
    fprintf(f, "%d", idx);
    fclose(f);
}

static void diag_log_task(void *arg)
{
    // Determina il file di questa sessione una sola volta, con un breve
    // montaggio dedicato solo a leggere/aggiornare l'indice di rotazione -
    // se la SD non e' disponibile ora, il log su SD resta disattivato per
    // tutto questo avvio (niente tentativi ripetuti ad ogni flush).
    char session_path[64] = {0};
    if (mount_sd()) {
        mkdir(LOG_DIR, 0755);
        int idx = read_next_index();
        snprintf(session_path, sizeof(session_path), "%s/boot_%d.log", LOG_DIR, idx);
        write_next_index((idx + 1) % ROTATE_COUNT);
        unmount_sd();
        ESP_LOGI(TAG, "Log diagnostico di questo avvio: %s", session_path);
    } else {
        ESP_LOGW(TAG, "SD non disponibile, log diagnostico su SD disattivato per questo avvio");
    }

    uint8_t buf[512];
    size_t session_bytes = 0;

    while (1) {
        vTaskDelay(pdMS_TO_TICKS(FLUSH_INTERVAL_MS));

        if (session_path[0] == '\0' || session_bytes >= MAX_FILE_BYTES || xStreamBufferIsEmpty(s_stream)) {
            continue;
        }
        if (!mount_sd()) {
            continue; // SD occupata da un'altra funzione in questo momento, si ritenta al giro dopo
        }
        FILE *f = fopen(session_path, "a");
        if (f) {
            size_t n;
            while (session_bytes < MAX_FILE_BYTES &&
                   (n = xStreamBufferReceive(s_stream, buf, sizeof(buf), 0)) > 0) {
                size_t to_write = n;
                if (session_bytes + to_write > MAX_FILE_BYTES) {
                    to_write = MAX_FILE_BYTES - session_bytes;
                }
                fwrite(buf, 1, to_write, f);
                session_bytes += to_write;
            }
            fclose(f);
        }
        unmount_sd();
    }
}

void diag_log_start(void)
{
    s_stream = xStreamBufferCreate(4096, 1);
    log_buffer_set_sink(s_stream);
    xTaskCreate(diag_log_task, "diag_log", 4096, NULL, 2, NULL);
}
