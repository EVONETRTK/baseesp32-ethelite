#pragma once

#include <stdbool.h>
#include "settings.h"

// Riconoscimento automatico del ricevitore (gnss_chip = GNSS_CHIP_AUTO).
//
// Da chiamare all'avvio PRIMA di gnss_io_init(): prova l'I2C (u-blox a 0x42,
// risultato di gnss_i2c_probe() in i2c_found) e la seriale, chiedendo la
// versione in UBX (UBX-MON-VER) e in ASCII Unicore (VERSIONA) a piu'
// velocita', cominciando da quella dell'ultimo ricevitore trovato. Aggiorna
// SOLO la copia di avvio *boot: gnss_chip (quello trovato), gnss_uart_baud,
// gnss_i2c. Nelle impostazioni salvate resta "automatico", cosi' un
// ricevitore cambiato viene riconosciuto al riavvio successivo.
// Bynav e Quectel non si riconoscono: vanno scelti a mano.
void gnss_detect_run(app_settings_t *boot, bool i2c_found);

// Ricevitore in uso: quello scelto, oppure quello trovato se "automatico"
// (u-blox se non ha risposto nessuno, come prima di questa funzione).
gnss_chip_t gnss_detect_effective(gnss_chip_t configured);

// Esito per il pannello ("" se il riconoscimento non e' stato usato).
const char *gnss_detect_note(void);
