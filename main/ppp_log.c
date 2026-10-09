#include "ppp_log.h"
#include "license.h"
#include "sd_mutex.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"

static const char *TAG = "ppp_log";

#define MOUNT_POINT   "/sdcard"
#define LOG_FILENAME  MOUNT_POINT "/ppp_log.rtcm3"

static StreamBufferHandle_t s_feed_stream;
static volatile bool s_should_record;

static SemaphoreHandle_t s_status_mutex;
static bool s_recording;
static uint64_t s_bytes_written;
static int64_t s_started_at_us;
static bool s_file_exists;

// Bus SPI dedicato (SPI3_HOST), stesso schema di sd_update.c - non
// condiviso con l'Ethernet W5500 (SPI2_HOST). Montato solo per la durata
// di una registrazione (o di un download), non tenuto sempre attivo: la
// SD resta libera per sd_update.c il resto del tempo.
static sdmmc_card_t *s_card;
static bool s_sd_mounted;

static void status_mutex_init(void)
{
    if (!s_status_mutex) {
        s_status_mutex = xSemaphoreCreateMutex();
    }
}

// sd_mutex_take() e' preso qui (non nel chiamante) e rilasciato in
// unmount_sd() - o subito, se il montaggio stesso fallisce - vedi
// sd_mutex.h per il motivo (contesa reale con altri moduli SD).
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
        ESP_LOGE(TAG, "Init bus SPI per SD fallita: %s", esp_err_to_name(err));
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
    err = sd_mount_retry(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Scheda SD non montata: %s", esp_err_to_name(err));
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

// Unico task che possiede il FILE* di scrittura: apre/chiude la SD in
// risposta a s_should_record (impostato da ppp_log_start()/stop(), letto
// qui al massimo ogni 500ms) ed e' l'unico a scrivere byte sul file,
// cosi' non serve nessun lock sul FILE* stesso (solo sullo status
// condiviso con la UI, protetto da s_status_mutex).
// I dati si accumulano in memoria e vengono scritti sulla SD a blocchi
// (monta, aggiunge, smonta) invece di tenerla montata per tutta la
// registrazione: con il ricevitore via I2C, finche' la SD e' montata il
// ricevitore non puo' comunicare (clock della SD e linea I2C condividono un
// filo sulla base di prova, vedi gnss_io.c), quindi una registrazione con la
// SD sempre montata non riceveva nulla da registrare (misurato: 0 byte).
// Una scrittura ogni 10 s ferma il ricevitore per pochi decimi di secondo,
// che il suo buffer assorbe. Utile anche per l'usura della scheda.
#define PPP_CHUNK_BYTES     (12 * 1024)
#define PPP_FLUSH_EVERY_US  (10LL * 1000 * 1000)
#define PPP_MIN_FREE_MB     50                                  // sotto: la registrazione si ferma
#define PPP_MAX_FILE_BYTES  (3500ULL * 1024 * 1024)             // margine sotto i 4 GB del FAT32

static bool ppp_append(const uint8_t *data, size_t len, bool truncate)
{
    if (!mount_sd()) {
        return false;
    }
    FILE *f = fopen(LOG_FILENAME, truncate ? "wb" : "ab");
    if (!f) {
        ESP_LOGE(TAG, "Impossibile aprire %s in scrittura", LOG_FILENAME);
        unmount_sd();
        return false;
    }
    size_t written = len ? fwrite(data, 1, len, f) : 0;
    fclose(f);
    uint64_t total_b = 0, free_b = 0;
    bool low_space = esp_vfs_fat_info(MOUNT_POINT, &total_b, &free_b) == ESP_OK &&
                     free_b < (uint64_t) PPP_MIN_FREE_MB * 1024 * 1024;
    unmount_sd();
    uint64_t file_bytes;
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    s_bytes_written += written;
    file_bytes = s_bytes_written;
    xSemaphoreGive(s_status_mutex);
    // Nessun limite di durata: una registrazione dimenticata accesa riempiva
    // la microSD (~100-150 MB al giorno) fino al limite di 4 GB per file del
    // FAT32, bloccando anche la copia della configurazione e l'archivio.
    if (s_should_record && (low_space || file_bytes >= PPP_MAX_FILE_BYTES)) {
        ESP_LOGW(TAG, "Registrazione PPP fermata da sola: %s",
                 low_space ? "microSD quasi piena" : "file al limite di dimensione del FAT32");
        s_should_record = false;
    }
    return written == len;
}

static void ppp_log_task(void *arg)
{
    uint8_t buf[512];
    uint8_t *chunk = NULL;
    size_t chunk_len = 0;
    int64_t last_flush_us = 0;

    while (1) {
        size_t n = xStreamBufferReceive(s_feed_stream, buf, sizeof(buf), pdMS_TO_TICKS(500));

        if (s_should_record && !s_recording) {
            chunk = malloc(PPP_CHUNK_BYTES);
            // File nuovo e vuoto: le scritture successive si aggiungono.
            if (!chunk || !ppp_append(NULL, 0, true)) {
                ESP_LOGE(TAG, "Registrazione PPP non avviata (%s)", chunk ? "SD non disponibile" : "memoria insufficiente");
                free(chunk);
                chunk = NULL;
                s_should_record = false;
                continue;
            }
            chunk_len = 0;
            last_flush_us = esp_timer_get_time();
            ESP_LOGI(TAG, "Registrazione PPP avviata: %s (scrittura a blocchi ogni 10 s)", LOG_FILENAME);
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_recording = true;
            s_bytes_written = 0;
            s_started_at_us = esp_timer_get_time();
            xSemaphoreGive(s_status_mutex);
        }

        if (s_recording && n > 0) {
            size_t take = n;
            if (chunk_len + take > PPP_CHUNK_BYTES) {
                take = PPP_CHUNK_BYTES - chunk_len; // il resto andrebbe perso: non dovrebbe succedere, flush sotto
            }
            memcpy(chunk + chunk_len, buf, take);
            chunk_len += take;
        }

        bool stopping = s_recording && !s_should_record;
        if (s_recording && chunk_len > 0 &&
            (stopping || chunk_len >= PPP_CHUNK_BYTES - sizeof(buf) ||
             esp_timer_get_time() - last_flush_us >= PPP_FLUSH_EVERY_US)) {
            if (!ppp_append(chunk, chunk_len, false)) {
                ESP_LOGW(TAG, "Scrittura di %u byte sulla SD fallita", (unsigned) chunk_len);
            }
            chunk_len = 0;
            last_flush_us = esp_timer_get_time();
        }

        if (stopping) {
            free(chunk);
            chunk = NULL;
            ESP_LOGI(TAG, "Registrazione PPP fermata (%llu byte)", (unsigned long long) s_bytes_written);
            xSemaphoreTake(s_status_mutex, portMAX_DELAY);
            s_recording = false;
            s_file_exists = true;
            xSemaphoreGive(s_status_mutex);
        }
    }
}

bool ppp_log_start(void)
{
    if (!license_has(LIC_BASE)) {
        return false;
    }
    status_mutex_init();
    if (!s_feed_stream) {
        s_feed_stream = xStreamBufferCreate(4096, 1);
        xTaskCreate(ppp_log_task, "ppp_log", 4096, NULL, 4, NULL);
    }
    s_should_record = true;
    return true;
}

bool ppp_log_stop(void)
{
    bool was = s_should_record;
    s_should_record = false;
    return was;
}

ppp_log_status_t ppp_log_get_status(void)
{
    ppp_log_status_t st = {0};
    if (!s_status_mutex) {
        return st;
    }
    xSemaphoreTake(s_status_mutex, portMAX_DELAY);
    st.recording = s_recording;
    st.bytes_written = s_bytes_written;
    st.started_at_us = s_started_at_us;
    st.file_exists = s_file_exists || s_recording;
    xSemaphoreGive(s_status_mutex);
    return st;
}

void ppp_log_feed(const uint8_t *data, size_t len)
{
    if (!s_feed_stream || !s_should_record) {
        return;
    }
    // Non bloccante: se il buffer e' pieno (task di scrittura in ritardo
    // sulla SD) i byte in eccesso vengono scartati invece di rallentare il
    // chiamante (gnss_uart_task, sul percorso critico verso il caster).
    xStreamBufferSend(s_feed_stream, data, len, 0);
}

FILE *ppp_log_open_for_read(void)
{
    if (s_should_record || s_recording) {
        ESP_LOGW(TAG, "Download rifiutato: registrazione in corso, ferma prima");
        return NULL;
    }
    if (!mount_sd()) {
        return NULL;
    }
    FILE *f = fopen(LOG_FILENAME, "rb");
    if (!f) {
        ESP_LOGW(TAG, "Nessun file di log da scaricare");
        unmount_sd();
        return NULL;
    }
    return f;
}

void ppp_log_close_for_read(FILE *f)
{
    if (f) {
        fclose(f);
    }
    unmount_sd();
}

#define PPP_DL_CHUNK (32 * 1024)

esp_err_t ppp_log_send_http(httpd_req_t *req)
{
    static const char *busy = "Nessun log disponibile, oppure una registrazione e' ancora in corso (fermala prima di scaricare)";
    if (s_should_record || s_recording) {
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, busy);
    }
    size_t buf_size = PPP_DL_CHUNK;
    uint8_t *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf_size = 4096; // senza PSRAM: blocchi piccoli nella RAM interna
        buf = malloc(buf_size);
    }
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    // A blocchi, montando e smontando la SD ogni volta (come raw_log_send_file):
    // con la SD montata l'I2C del ricevitore e' fermo, quindi niente montaggi
    // lunghi mentre il file (anche centinaia di MB) viaggia in rete.
    long offset = 0;
    bool started = false;
    esp_err_t err = ESP_OK;
    while (1) {
        if (s_should_record || s_recording) {
            // Registrazione avviata durante il download: il file verrebbe
            // troncato sotto i piedi, il download si interrompe.
            ESP_LOGW(TAG, "Download interrotto: e' partita una nuova registrazione");
            err = ESP_FAIL;
            break;
        }
        if (!mount_sd()) {
            err = ESP_FAIL;
            break;
        }
        FILE *f = fopen(LOG_FILENAME, "rb");
        size_t n = 0;
        if (f) {
            if (fseek(f, offset, SEEK_SET) == 0) {
                n = fread(buf, 1, buf_size, f);
            }
            fclose(f);
        }
        unmount_sd();
        if (!f && !started) {
            free(buf);
            httpd_resp_set_status(req, "409 Conflict");
            httpd_resp_set_type(req, "text/plain; charset=utf-8");
            return httpd_resp_sendstr(req, busy);
        }
        if (n == 0) {
            break;
        }
        if (!started) {
            httpd_resp_set_type(req, "application/octet-stream");
            httpd_resp_set_hdr(req, "Content-Disposition", "attachment; filename=\"ppp_log.rtcm3\"");
            started = true;
        }
        offset += (long) n;
        if (httpd_resp_send_chunk(req, (const char *) buf, n) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // spazio all'I2C tra un blocco e l'altro
    }
    free(buf);
    if (!started) {
        // File vuoto o SD non montabile al primo blocco.
        httpd_resp_set_status(req, "409 Conflict");
        httpd_resp_set_type(req, "text/plain; charset=utf-8");
        return httpd_resp_sendstr(req, busy);
    }
    httpd_resp_send_chunk(req, NULL, 0); // chiude la risposta chunked
    return err;
}
