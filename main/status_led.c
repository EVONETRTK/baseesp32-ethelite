#include "status_led.h"
#include "status.h"
#include "rgb_led.h"
#include "sdkconfig.h"

#include <stdbool.h>
#include "driver/gpio.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define NET_PIN  CONFIG_BASEESP32_LED_NET_PIN
#define DATA_PIN CONFIG_BASEESP32_LED_DATA_PIN

// I bool di Kconfig, quando disattivati, non generano nessuna #define (non
// "definito a 0"): usarli direttamente in un'espressione C, invece che in
// un #if, fallisce con "undeclared identifier". Ridefiniamo qui un vero 0/1.
#ifdef CONFIG_BASEESP32_LED_ACTIVE_LOW
#define ACTIVE_LOW 1
#else
#define ACTIVE_LOW 0
#endif

static void led_set(int pin, bool on)
{
    if (pin < 0) {
        return;
    }
    gpio_set_level(pin, ACTIVE_LOW ? !on : on);
}

static void configure_output(int pin)
{
    if (pin < 0) {
        return;
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << pin,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);
    led_set(pin, false);
}

// LED rete: acceso fisso se WiFi o cellulare sono su, lampeggiante
// (~300ms) finche' il dispositivo sta ancora cercando una rete. LED dati:
// si accende per un ciclo (~150ms) ad ogni volta che il contatore RTCM
// condiviso avanza. LED RGB (se configurato): stesso concetto ma a
// colori - blu=WiFi, verde=cellulare, spento/lampeggiante=nessuna rete,
// con un lampo bianco sovrapposto ad ogni pacchetto dati.
static void status_led_task(void *arg)
{
    uint32_t last_bytes = status_get_rtcm_total_bytes();
    bool data_led_on = false;
    bool net_blink_state = false;

    while (1) {
        net_status_t net = status_get_net();
        bool net_up = (net != NET_STATUS_NONE);

        if (net_up) {
            led_set(NET_PIN, true);
        } else {
            net_blink_state = !net_blink_state;
            led_set(NET_PIN, net_blink_state);
        }

        uint32_t bytes = status_get_rtcm_total_bytes();
        bool data_flash = false;
        if (bytes != last_bytes) {
            last_bytes = bytes;
            led_set(DATA_PIN, true);
            data_led_on = true;
            data_flash = true;
        } else if (data_led_on) {
            led_set(DATA_PIN, false);
            data_led_on = false;
        }

        if (data_flash) {
            rgb_led_set(255, 255, 255);
        } else if (net == NET_STATUS_WIFI) {
            rgb_led_set(0, 0, 255);
        } else if (net == NET_STATUS_CELLULAR) {
            rgb_led_set(0, 255, 0);
        } else {
            rgb_led_set(net_blink_state ? 40 : 0, 0, 0);
        }

        vTaskDelay(pdMS_TO_TICKS(150));
    }
}

void status_led_start(void)
{
    rgb_led_init();

    if (NET_PIN < 0 && DATA_PIN < 0) {
        return;
    }
    configure_output(NET_PIN);
    configure_output(DATA_PIN);
    xTaskCreate(status_led_task, "status_led", 2048, NULL, 2, NULL);
}
