#pragma once

// Configura (se abilitato in Kconfig, BASEESP32_RESET_BUTTON_PIN) il
// pulsante di reset configurazione e avvia il task che lo monitora:
// tenendolo premuto (verso massa) per BASEESP32_RESET_BUTTON_HOLD_MS
// cancella tutta la configurazione salvata in NVS e riavvia il
// dispositivo con i default di fabbrica. Non fa nulla se il pin e' -1.
void reset_button_start(void);
