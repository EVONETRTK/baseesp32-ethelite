#pragma once

#include <stdint.h>

// Inizializza il LED RGB in base al tipo e ai pin scelti in settings
// (RGB_LED_NONE/WS2812/PWM3, vedi settings.h) - impostabili dalla UI web,
// non serve ricompilare. Non fa nulla se RGB_LED_NONE o pin mancanti.
void rgb_led_init(void);

// Imposta il colore corrente (0-255 per canale). Non fa nulla se non
// inizializzato/nessun LED RGB configurato.
void rgb_led_set(uint8_t r, uint8_t g, uint8_t b);
