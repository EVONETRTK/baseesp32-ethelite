#include "reset_button.h"
#include "sdkconfig.h"

#include "driver/gpio.h"
#include "esp_system.h"
#include "esp_log.h"
#include "nvs_flash.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "reset_button";

#define BUTTON_PIN CONFIG_BASEESP32_RESET_BUTTON_PIN
#define HOLD_MS    CONFIG_BASEESP32_RESET_BUTTON_HOLD_MS
#define POLL_MS    100

static void erase_settings_and_reboot(void)
{
    ESP_LOGW(TAG, "Reset configurazione richiesto dal pulsante: cancello NVS e riavvio");
    nvs_flash_erase();
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

    xTaskCreate(reset_button_task, "reset_button", 2048, NULL, 2, NULL);
}
