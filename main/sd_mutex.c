#include "sd_mutex.h"

static SemaphoreHandle_t s_sd_mutex;

void sd_mutex_init(void)
{
    s_sd_mutex = xSemaphoreCreateRecursiveMutex();
}

void sd_mutex_take(void)
{
    xSemaphoreTakeRecursive(s_sd_mutex, portMAX_DELAY);
}

void sd_mutex_give(void)
{
    xSemaphoreGiveRecursive(s_sd_mutex);
}

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"

#define SD_MOUNT_ATTEMPTS  3
#define SD_MOUNT_RETRY_MS  1500
// Dopo un montaggio fallito, per questo tempo i moduli successivi fanno un
// solo tentativo: con la scheda davvero assente o guasta ogni modulo
// (sd_update, fw_archive, diag_log...) riprovava per 3 s, ritardando l'avvio.
#define SD_FAIL_MEMORY_US  (60LL * 1000 * 1000)

static int64_t s_last_fail_us = -1;

esp_err_t sd_mount_retry(const char *base_path, const sdmmc_host_t *host,
                         const sdspi_device_config_t *slot, const esp_vfs_fat_mount_config_t *cfg,
                         sdmmc_card_t **out_card)
{
    esp_err_t err = ESP_FAIL;
    int64_t now = esp_timer_get_time();
    int attempts = (s_last_fail_us >= 0 && now - s_last_fail_us < SD_FAIL_MEMORY_US) ? 1 : SD_MOUNT_ATTEMPTS;
    for (int attempt = 1; attempt <= attempts; attempt++) {
        err = esp_vfs_fat_sdspi_mount(base_path, host, slot, cfg, out_card);
        if (err != ESP_ERR_TIMEOUT || attempt == attempts) {
            break;
        }
        ESP_LOGW("sd_mount", "La scheda SD non risponde, riprovo tra %d ms (%d/%d)",
                 SD_MOUNT_RETRY_MS, attempt + 1, SD_MOUNT_ATTEMPTS);
        vTaskDelay(pdMS_TO_TICKS(SD_MOUNT_RETRY_MS));
    }
    s_last_fail_us = (err == ESP_OK) ? -1 : esp_timer_get_time();
    return err;
}

bool sd_mutex_try_take(TickType_t ticks)
{
    return xSemaphoreTakeRecursive(s_sd_mutex, ticks) == pdTRUE;
}

// Task che ha la SD montata e lo ha segnalato (vedi sd_mutex.h). Scritto e
// letto solo con sd_mutex preso.
static TaskHandle_t s_mount_owner;

void sd_mount_note(bool mounted)
{
    s_mount_owner = mounted ? xTaskGetCurrentTaskHandle() : NULL;
}

bool sd_mounted_by_me(void)
{
    return s_mount_owner != NULL && s_mount_owner == xTaskGetCurrentTaskHandle();
}
