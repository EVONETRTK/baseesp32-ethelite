#include "reset_button.h"
#include "sys_stats.h"
#include "settings.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"
#include "nvs.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "reset_button";

#define BUTTON_PIN CONFIG_BASEESP32_RESET_BUTTON_PIN
#define HOLD_MS    CONFIG_BASEESP32_RESET_BUTTON_HOLD_MS
#define POLL_MS    100

static void erase_settings_and_reboot(void)
{
    ESP_LOGW(TAG, "Reset configurazione richiesto dal pulsante: cancello la NVS (tranne la licenza) e riavvio");
    // Motivo del riavvio e avvio segnato come completato (memoria RTC): un
    // reset di fabbrica durante l'avvio non e' un "avvio fermo".
    sys_stats_note_restart_reason("reset di fabbrica dal pulsante");
    // Non piu' nvs_flash_erase(): cancellava anche la licenza e la chiave del
    // ricevitore, che andava riattivato dal server (09/10/2026).
    settings_factory_erase_keep_license();
    // Segno per config_backup.c: e' un reset voluto, la copia della
    // configurazione sulla microSD non va ripristinata.
    nvs_handle_t h;
    if (nvs_open("cfgbk", NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, "skip", 1);
        nvs_commit(h);
        nvs_close(h);
    }
    esp_restart();
}

static void reset_button_task(void *arg)
{
    uint32_t held_ms = 0;

    while (1) {
        // Pulsante verso massa con pull-up interno: premuto = livello basso.
        if (gpio_get_level(BUTTON_PIN) == 0) {
            held_ms += POLL_MS;
            if (held_ms >= HOLD_MS) {
                erase_settings_and_reboot();
            }
        } else {
            held_ms = 0;
        }
        vTaskDelay(pdMS_TO_TICKS(POLL_MS));
    }
}

void reset_button_start(void)
{
    // Pin passato per variabile (non usato direttamente nello shift sotto):
    // BUTTON_PIN e' una costante di compilazione e puo' valere -1 di
    // default (disabilitato), il che genererebbe un warning "shift count
    // negative" se scritta direttamente in "1ULL << BUTTON_PIN".
    int pin = BUTTON_PIN;
    if (pin < 0) {
        return;
    }

    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io_conf);

    // 4096 (era 2048): a riposo il task legge solo il pin, ma il reset fa log
    // (log_buffer: riga da 256 byte + vprintf), operazioni NVS e il giro dei
    // namespace - con 2048 il reset rischiava un overflow dello stack prima
    // di cancellare qualcosa.
    xTaskCreate(reset_button_task, "reset_button", 4096, NULL, 2, NULL);
}
