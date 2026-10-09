#include "raw_log.h"
#include "license.h"
#include "sd_mutex.h"
#include "time_sync.h"

#include <stdio.h>
#include <string.h>
#include <time.h>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_heap_caps.h"
#include "esp_vfs_fat.h"
#include "driver/sdspi_host.h"
#include "driver/spi_common.h"
#include "sdmmc_cmd.h"
#include "nvs.h"
#include "cJSON.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "raw_log";

#define MOUNT_POINT   "/sdcard"
#define RAW_DIR       MOUNT_POINT "/raw"
#define RING_BYTES    (512 * 1024)       // nella PSRAM: ~14 min di dati se la SD non e' disponibile
#define WRITE_CHUNK   (32 * 1024)
#define MAX_PAYLOAD   8192               // RAWX = 16 + 32 * misure: con 4 costellazioni anche oltre 2,5 KB
#define MIN_FREE_MB   20                 // sotto questo spazio libero la sessione si ferma
#define NVS_NS        "rawlog"

static StreamBufferHandle_t s_ring;
static uint8_t *s_chunk;                 // buffer di scrittura/lettura (PSRAM)
static sdmmc_card_t *s_card;

static volatile bool s_active;           // sessione in corso
static volatile bool s_capture;          // frame accettati nel buffer (sessione attiva e file aperto)
static uint8_t s_interval_s;
static int64_t s_end_unix;
static char s_file[64];
static uint64_t s_bytes;
static volatile uint32_t s_frames, s_dropped;
static char s_note[96];

// --- parser UBX (solo nel task che legge il ricevitore) ---------------------------
static uint8_t s_frame[6 + MAX_PAYLOAD + 2];
static size_t s_pos, s_need;

static void set_note(const char *n)
{
    strlcpy(s_note, n, sizeof(s_note));
}

void raw_log_feed(const uint8_t *buf, size_t len)
{
    if (!s_capture) {
        s_pos = 0;
        return;
    }
    for (size_t i = 0; i < len; i++) {
        uint8_t c = buf[i];
        if (s_pos == 0) {
            if (c == 0xB5) {
                s_frame[s_pos++] = c;
            }
            continue;
        }
        if (s_pos == 1) {
            if (c == 0x62) {
                s_frame[s_pos++] = c;
            } else {
                s_pos = (c == 0xB5) ? 1 : 0;
            }
            continue;
        }
        s_frame[s_pos++] = c;
        if (s_pos == 6) {
            size_t plen = (size_t) s_frame[4] | ((size_t) s_frame[5] << 8);
            // Solo RXM-RAWX (0x02 0x15) e RXM-SFRBX (0x02 0x13): il resto si salta.
            bool wanted = s_frame[2] == 0x02 && (s_frame[3] == 0x15 || s_frame[3] == 0x13);
            if (!wanted || plen > MAX_PAYLOAD) {
                s_pos = 0;
                continue;
            }
            s_need = 6 + plen + 2;
        }
        if (s_pos >= 6 && s_pos == s_need) {
            uint8_t a = 0, b = 0;
            for (size_t k = 2; k < s_need - 2; k++) {
                a += s_frame[k];
                b += a;
            }
            if (a == s_frame[s_need - 2] && b == s_frame[s_need - 1]) {
                s_frames++;
                if (xStreamBufferSpacesAvailable(s_ring) >= s_need) {
                    xStreamBufferSend(s_ring, s_frame, s_need, 0);
                } else {
                    s_dropped++;
                }
            }
            s_pos = 0;
        }
    }
}

// --- microSD (stesso schema degli altri moduli: sd_mutex preso fino allo smontaggio) ---
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

// --- sessione ------------------------------------------------------------------
static void nvs_save_session(int64_t end_unix, uint8_t interval_s)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "end", end_unix);
        nvs_set_u8(h, "int", interval_s);
        nvs_commit(h);
        nvs_close(h);
    }
}

void raw_log_init(void)
{
    uint8_t *ring_mem = heap_caps_malloc(RING_BYTES, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    static StaticStreamBuffer_t ring_struct;
    s_chunk = heap_caps_malloc(WRITE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!ring_mem || !s_chunk) {
        ESP_LOGW(TAG, "PSRAM non disponibile: registrazione dei dati grezzi disattivata");
        free(ring_mem);
        free(s_chunk);
        s_chunk = NULL;
        return;
    }
    s_ring = xStreamBufferCreateStatic(RING_BYTES - 1, 1, ring_mem, &ring_struct);
    // Sessione in corso prima del riavvio: riprende (nuovo file) se non e' finita.
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        int64_t end = 0;
        uint8_t iv = 5;
        nvs_get_i64(h, "end", &end);
        nvs_get_u8(h, "int", &iv);
        nvs_close(h);
        if (end > 0 && !license_has(LIC_BASE)) {
            ESP_LOGW(TAG, "Sessione di registrazione non ripresa: Pacchetto Base non compreso nella licenza");
        } else if (end > 0) {
            s_end_unix = end;
            s_interval_s = iv ? iv : 5;
            s_active = true; // il file si apre quando c'e' l'ora esatta (raw_log_service)
            ESP_LOGI(TAG, "Sessione di registrazione dei dati grezzi da riprendere (fine prevista salvata)");
        }
    }
}

bool raw_log_active(void)
{
    return s_active;
}

uint8_t raw_log_wanted_rate(void)
{
    return s_active ? s_interval_s : 0;
}

bool raw_log_start(uint32_t hours, uint8_t interval_s, char *err, size_t err_size)
{
    if (!license_has(LIC_BASE)) {
        snprintf(err, err_size, "funzione non compresa nella licenza");
        return false;
    }
    if (!s_ring) {
        snprintf(err, err_size, "PSRAM non disponibile");
        return false;
    }
    if (!time_sync_is_valid()) {
        snprintf(err, err_size, "ora esatta non ancora disponibile (NTP): riprova tra poco");
        return false;
    }
    if (hours < 1 || hours > 72 || !(interval_s == 1 || interval_s == 5 || interval_s == 30)) {
        snprintf(err, err_size, "durata (1-72 h) o intervallo (1, 5, 30 s) non validi");
        return false;
    }
    if (s_active) {
        snprintf(err, err_size, "registrazione gia' in corso");
        return false;
    }
    // Sessione fermata ma file non ancora chiuso: raw_log_service sta ancora
    // scrivendo il resto del buffer sulla microSD. Ripartire ora azzererebbe
    // il nome del file e i contatori, e la coda finirebbe nel file nuovo.
    if (s_file[0]) {
        snprintf(err, err_size, "la registrazione precedente si sta ancora chiudendo sulla microSD: riprova tra 30 secondi");
        return false;
    }
    s_interval_s = interval_s;
    s_end_unix = (int64_t) time(NULL) + (int64_t) hours * 3600;
    s_file[0] = '\0';
    s_bytes = 0;
    s_frames = 0;
    s_dropped = 0;
    set_note("");
    nvs_save_session(s_end_unix, s_interval_s);
    s_active = true;
    ESP_LOGI(TAG, "Registrazione dei dati grezzi avviata: %lu h, misure ogni %u s", (unsigned long) hours, interval_s);
    return true;
}

void raw_log_stop(const char *why)
{
    if (!s_active) {
        return;
    }
    s_active = false; // il resto del buffer lo scrive raw_log_service, poi chiude
    nvs_save_session(0, s_interval_s);
    set_note(why);
    ESP_LOGI(TAG, "Registrazione dei dati grezzi fermata: %s", why);
}

static bool open_new_file(void)
{
    time_t now = time(NULL);
    struct tm tm;
    localtime_r(&now, &tm);
    snprintf(s_file, sizeof(s_file), "EVO_%04d%02d%02d_%02d%02d.ubx",
             tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min);
    xStreamBufferReset(s_ring);
    s_capture = true;
    ESP_LOGI(TAG, "File dei dati grezzi: %s/%s", RAW_DIR, s_file);
    return true;
}

void raw_log_service(void)
{
    if (!s_ring) {
        return;
    }
    if (s_active && !s_file[0]) {
        if (!time_sync_is_valid()) {
            return; // nome del file con data e ora: si aspetta l'NTP
        }
        if ((int64_t) time(NULL) >= s_end_unix) {
            raw_log_stop("sessione gia' scaduta al riavvio");
            return;
        }
        open_new_file();
        return;
    }
    if (s_active && (int64_t) time(NULL) >= s_end_unix) {
        raw_log_stop("completata (durata raggiunta)");
    }
    if (!s_file[0]) {
        return;
    }
    size_t pending = xStreamBufferBytesAvailable(s_ring);
    if (pending == 0 && s_active) {
        return;
    }
    if (pending && mount_sd()) {
        mkdir(RAW_DIR, 0755);
        char path[96];
        snprintf(path, sizeof(path), "%s/%s", RAW_DIR, s_file);
        FILE *f = fopen(path, "ab");
        bool ok = f != NULL;
        while (ok && (pending = xStreamBufferReceive(s_ring, s_chunk, WRITE_CHUNK, 0)) > 0) {
            ok = fwrite(s_chunk, 1, pending, f) == pending;
            s_bytes += pending;
        }
        if (f) {
            ok = (fclose(f) == 0) && ok;
        }
        uint64_t total = 0, free_b = 0;
        bool full = esp_vfs_fat_info(MOUNT_POINT, &total, &free_b) == ESP_OK && free_b < (uint64_t) MIN_FREE_MB * 1024 * 1024;
        unmount_sd();
        if (!ok) {
            ESP_LOGW(TAG, "Scrittura del file dei dati grezzi fallita");
            set_note("errore di scrittura sulla microSD");
        }
        if (full && s_active) {
            raw_log_stop("microSD quasi piena");
        }
    }
    if (!s_active) {
        // Sessione finita: buffer svuotato, il file e' completo.
        s_capture = false;
        ESP_LOGI(TAG, "File %s chiuso: %llu byte, %lu frame, %lu persi", s_file,
                 (unsigned long long) s_bytes, (unsigned long) s_frames, (unsigned long) s_dropped);
        s_file[0] = '\0';
    }
}

void raw_log_get_status(raw_log_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->active = s_active;
    out->waiting_time = s_active && !s_file[0];
    strlcpy(out->file, s_file, sizeof(out->file));
    out->bytes = s_bytes + (s_ring ? xStreamBufferBytesAvailable(s_ring) : 0);
    out->frames = s_frames;
    out->dropped = s_dropped;
    out->end_unix = s_end_unix;
    out->interval_s = s_interval_s;
    strlcpy(out->note, s_note, sizeof(out->note));
}

// --- elenco, scaricamento, cancellazione ------------------------------------------
static bool valid_name(const char *name)
{
    size_t n = strlen(name);
    if (n < 5 || n > 40 || strcmp(name + n - 4, ".ubx") != 0) {
        return false;
    }
    for (size_t i = 0; i < n; i++) {
        char c = name[i];
        if (!((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '_' || c == '.')) {
            return false; // niente "/" o "..": solo file della cartella raw
        }
    }
    return true;
}

esp_err_t raw_log_send_list(httpd_req_t *req)
{
    cJSON *root = cJSON_CreateObject();
    cJSON *arr = cJSON_AddArrayToObject(root, "files");
    bool sd_ok = mount_sd();
    if (sd_ok) {
        DIR *d = opendir(RAW_DIR);
        struct dirent *e;
        while (d && (e = readdir(d)) != NULL) {
            if (!valid_name(e->d_name)) {
                continue;
            }
            char path[96];
            snprintf(path, sizeof(path), "%s/%.48s", RAW_DIR, e->d_name); // valid_name: al massimo 40 caratteri
            struct stat st;
            if (stat(path, &st) == 0) {
                cJSON *o = cJSON_CreateObject();
                cJSON_AddStringToObject(o, "name", e->d_name);
                cJSON_AddNumberToObject(o, "size", (double) st.st_size);
                cJSON_AddItemToArray(arr, o);
            }
        }
        if (d) {
            closedir(d);
        }
        uint64_t total = 0, free_b = 0;
        if (esp_vfs_fat_info(MOUNT_POINT, &total, &free_b) == ESP_OK) {
            cJSON_AddNumberToObject(root, "free_bytes", (double) free_b);
        }
        unmount_sd();
    }
    cJSON_AddBoolToObject(root, "sd_ok", sd_ok);
    char *json = cJSON_PrintUnformatted(root);
    cJSON_Delete(root);
    if (!json) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    httpd_resp_set_type(req, "application/json");
    esp_err_t err = httpd_resp_sendstr(req, json);
    free(json);
    return err;
}

esp_err_t raw_log_send_file(httpd_req_t *req, const char *name)
{
    if (!valid_name(name)) {
        return httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "nome non valido");
    }
    uint8_t *buf = heap_caps_malloc(WRITE_CHUNK, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        return httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria insufficiente");
    }
    char path[96], disp[96];
    snprintf(path, sizeof(path), "%s/%s", RAW_DIR, name);
    snprintf(disp, sizeof(disp), "attachment; filename=\"%s\"", name);
    httpd_resp_set_type(req, "application/octet-stream");
    httpd_resp_set_hdr(req, "Content-Disposition", disp);
    // A blocchi, montando e smontando la SD ogni volta: con la SD montata
    // l'I2C del ricevitore e' fermo, quindi niente montaggi lunghi mentre il
    // file (anche decine di MB) viaggia in rete.
    long offset = 0;
    bool found = false;
    while (1) {
        if (!mount_sd()) {
            break;
        }
        FILE *f = fopen(path, "rb");
        size_t n = 0;
        if (f) {
            found = true;
            fseek(f, offset, SEEK_SET);
            n = fread(buf, 1, WRITE_CHUNK, f);
            fclose(f);
        }
        unmount_sd();
        if (n == 0) {
            break;
        }
        offset += (long) n;
        if (httpd_resp_send_chunk(req, (const char *) buf, n) != ESP_OK) {
            free(buf);
            return ESP_FAIL;
        }
        vTaskDelay(pdMS_TO_TICKS(20)); // spazio all'I2C tra un blocco e l'altro
    }
    free(buf);
    if (!found) {
        httpd_resp_set_status(req, "404 Not Found");
        httpd_resp_sendstr_chunk(req, "File non trovato o microSD non disponibile.\n");
    }
    return httpd_resp_send_chunk(req, NULL, 0);
}

bool raw_log_delete(const char *name)
{
    if (!valid_name(name) || (s_file[0] && strcmp(name, s_file) == 0)) {
        return false; // non si cancella il file della sessione in corso
    }
    if (!mount_sd()) {
        return false;
    }
    char path[96];
    snprintf(path, sizeof(path), "%s/%s", RAW_DIR, name);
    bool ok = unlink(path) == 0;
    unmount_sd();
    return ok;
}
