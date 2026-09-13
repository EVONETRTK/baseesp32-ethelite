#pragma once

// Configura (se abilitati in Kconfig, BASEESP32_LED_NET_PIN /
// BASEESP32_LED_DATA_PIN) i GPIO dei LED di stato rete e attivita' dati
// RTCM, e avvia il task che li pilota in base allo stato condiviso in
// status.c. Non fa nulla se entrambi i pin sono -1.
void status_led_start(void);
