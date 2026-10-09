#include "diag_log.h"
#include "settings.h"
#include "log_buffer.h"
#include "sd_mutex.h"
#include "config_backup.h"
#include "raw_log.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>
#include <unistd.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
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
static int s_session_idx = -1; // file di questo avvio (boot_N.log), -1 se SD assente

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
    err = sd_mount_retry(MOUNT_POINT, &host, &slot_cfg, &mount_cfg, &s_card);
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

static TaskHandle_t s_task;
static SemaphoreHandle_t s_flushed; // dato dal task dopo una scrittura richiesta

// Copia sulla microSD quello che il log ha in memoria (file di questo avvio).
// File con la parte precedente di questo stesso avvio: boot_N.log -> boot_N.prv.
static void prev_path_of(const char *session_path, char *out, size_t out_size)
{
    strlcpy(out, session_path, out_size);
    size_t l = strlen(out);
    if (l > 4 && strcmp(out + l - 4, ".log") == 0) {
        strlcpy(out + l - 4, ".prv", out_size - (l - 4));
    }
}

// Copia sulla microSD quello che il log ha in memoria (file di questo avvio).
// Arrivato a MAX_FILE_BYTES il file passa a boot_N.prv (sostituendo quello
// precedente) e si riparte da un file vuoto (09/10/2026): prima la scrittura
// si fermava per tutto il resto dell'avvio, e dopo qualche giorno di
// funzionamento un riavvio di sicurezza o un crash non lasciava sulla SD
// nulla delle ore precedenti. Restano sempre gli ultimi 200-400 KB.
static void diag_flush(const char *session_path, size_t *session_bytes, uint8_t *buf, size_t buf_size)
{
    if (session_path[0] == '\0' || xStreamBufferIsEmpty(s_stream)) {
        return;
    }
    if (!mount_sd()) {
        return; // SD occupata da un'altra funzione in questo momento, si ritenta al giro dopo
    }
    // Ogni volta: dopo "Formatta microSD" dal pannello la cartella non c'e' piu'
    // e il log smetteva di scrivere fino al riavvio.
    mkdir(LOG_DIR, 0755);
    FILE *f = fopen(session_path, "a");
    if (f) {
        size_t n;
        while ((n = xStreamBufferReceive(s_stream, buf, buf_size, 0)) > 0) {
            if (*session_bytes + n > MAX_FILE_BYTES) {
                fclose(f);
                char prev[64];
                prev_path_of(session_path, prev, sizeof(prev));
                unlink(prev);
                rename(session_path, prev);
                f = fopen(session_path, "w");
                *session_bytes = 0;
                if (!f) {
                    break;
                }
                static const char head[] = "--- continua: la parte precedente di questo avvio e' nel file .prv ---\n";
                fwrite(head, 1, sizeof(head) - 1, f);
                *session_bytes += sizeof(head) - 1;
            }
            fwrite(buf, 1, n, f);
            *session_bytes += n;
        }
        if (f) {
            fclose(f);
        }
    }
    unmount_sd();
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
        // File nuovo a ogni avvio: prima si apriva solo in aggiunta, quindi
        // a ogni giro della rotazione il file conteneva anche le sessioni di
        // 5 avvii prima e cresceva senza limite.
        FILE *trunc = fopen(session_path, "w");
        if (trunc) {
            fclose(trunc);
        }
        // La parte precedente lasciata da un vecchio avvio con lo stesso
        // indice non c'entra con questo.
        char prev[64];
        prev_path_of(session_path, prev, sizeof(prev));
        unlink(prev);
        s_session_idx = idx;
        unmount_sd();
        ESP_LOGI(TAG, "Log diagnostico di questo avvio: %s", session_path);
    } else {
        ESP_LOGW(TAG, "SD non disponibile, log diagnostico su SD disattivato per questo avvio");
    }

    uint8_t buf[512];
    size_t session_bytes = 0;

    while (1) {
        // Ogni 30 s, oppure subito se richiesto (diag_log_flush_now).
        // Con la registrazione dei dati grezzi attiva ogni 10 s: blocchi piccoli
        // sulla SD, cosi' l'I2C del ricevitore resta fermo poco ogni volta.
        bool forced = ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(raw_log_active() ? 10000 : FLUSH_INTERVAL_MS)) > 0;
        raw_log_service();
        diag_flush(session_path, &session_bytes, buf, sizeof(buf));
        config_backup_service(); // copia della configurazione, se richiesta (anche prima di un riavvio)
        if (forced) {
            xSemaphoreGive(s_flushed);
            continue;
        }
        {
            static UBaseType_t logged_min = 0xFFFF;
            UBaseType_t free_words = uxTaskGetStackHighWaterMark(NULL);
            if (free_words < logged_min) {
                logged_min = free_words;
                ESP_LOGI(TAG, "Stack libero minimo del task: %u byte", (unsigned) free_words);
            }
        }

    }
}

void diag_log_flush_now(uint32_t timeout_ms)
{
    if (!s_task || !s_flushed || xTaskGetCurrentTaskHandle() == s_task) {
        return;
    }
    xSemaphoreTake(s_flushed, 0); // scarta un segnale vecchio
    xTaskNotifyGive(s_task);
    xSemaphoreTake(s_flushed, pdMS_TO_TICKS(timeout_ms));
}

void diag_log_start(void)
{
    s_flushed = xSemaphoreCreateBinary();
    s_stream = xStreamBufferCreate(4096, 1);
    log_buffer_set_sink(s_stream);
    // 6144 (era 4096): dalla 1.19.84 questo task scrive anche la copia della
    // configurazione sulla SD (config_backup.c). Con 4096 lo stack era al
    // limite: i crash rari dopo gli aggiornamenti (driver WiFi, pthread)
    // comparivano subito dopo la copia. Lo stack libero e' nel log.
    xTaskCreate(diag_log_task, "diag_log", 6144, NULL, 2, &s_task);
}

int diag_log_list(diag_log_file_t *out, int max)
{
    int n = 0;
    if (!mount_sd()) {
        return -1;
    }
    for (int i = 0; i < ROTATE_COUNT && n < max; i++) {
        char path[64];
        snprintf(path, sizeof(path), "%s/boot_%d.log", LOG_DIR, i);
        struct stat st;
        if (stat(path, &st) == 0) {
            out[n].index = i;
            out[n].size = (uint32_t) st.st_size;
            out[n].current = (i == s_session_idx);
            n++;
        }
    }
    unmount_sd();
    return n;
}

// Toglie i codici colore ANSI (ESC [ ... m) pensati per il terminale.
// Lo stato resta tra un blocco e l'altro: un codice puo' essere spezzato.
static size_t strip_ansi(char *buf, size_t len, int *in_esc)
{
    size_t w = 0;
    for (size_t r = 0; r < len; r++) {
        char c = buf[r];
        if (*in_esc) {
            if (c == 'm') {
                *in_esc = 0;
            }
            continue;
        }
        if (c == 0x1b) {
            *in_esc = 1;
            continue;
        }
        buf[w++] = c;
    }
    return w;
}

esp_err_t diag_log_send_http(httpd_req_t *req, int index)
{
    if (index < 0 || index >= ROTATE_COUNT) {
        return httpd_resp_send_err(req, HTTPD_404_NOT_FOUND, "indice non valido");
    }
    char path[64];
    snprintf(path, sizeof(path), "%s/boot_%d.log", LOG_DIR, index);
    char *buf = malloc(4096);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    char disp[64];
    snprintf(disp, sizeof(disp), "attachment; filename=\"evonetrtk_boot_%d.log\"", index);
    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);

    // A blocchi da 4 KB, montando e smontando la SD ogni volta: con il
    // ricevitore via I2C, finche' la SD e' montata l'I2C e' fermo (vedi
    // gnss_io.c), quindi niente montaggi lunghi mentre il file viaggia in rete.
    long offset = 0;
    int in_esc = 0;
    bool found = false;
    while (1) {
        if (!mount_sd()) {
            break;
        }
        FILE *f = fopen(path, "r");
        size_t n = 0;
        if (f) {
            found = true;
            fseek(f, offset, SEEK_SET);
            n = fread(buf, 1, 4096, f);
            fclose(f);
        }
        unmount_sd();
        if (n == 0) {
            break;
        }
        offset += (long) n;
        size_t out = strip_ansi(buf, n, &in_esc);
        if (out && httpd_resp_send_chunk(req, buf, out) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // spazio all'I2C tra un blocco e l'altro
    }
    free(buf);
    if (!found) {
        httpd_resp_sendstr_chunk(req, "File non trovato o microSD non disponibile.\n");
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}
