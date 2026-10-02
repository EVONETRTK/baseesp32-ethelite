#pragma once

#include <stdbool.h>

// Copia di sicurezza della configurazione sulla microSD
// (/sdcard/config/settings.bin, stesso formato del blob NVS). Il 02/10/2026
// la configurazione della base di prova e' andata persa per un crash durante
// il salvataggio: con questa copia la base si ripristina da sola all'avvio.

// Chiede di aggiornare la copia (dopo ogni salvataggio). Non tocca la SD:
// la scrive config_backup_service() dal task del log diagnostico.
void config_backup_request(void);

// All'avvio, subito dopo settings_init(): se in NVS non c'era una
// configurazione valida, prova a ripristinarla dalla microSD e la ricarica.
// true se ha ripristinato.
bool config_backup_restore_if_missing(void);

// Chiede la prima copia dopo l'avvio.
void config_backup_start(void);

// Scrive la copia se richiesta. Chiamata dal task del log diagnostico
// (diag_log.c) ogni 30 s, con la SD non montata.
void config_backup_service(void);
