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

// Protezione dai riavvii a catena, all'avvio dopo
// config_backup_restore_if_missing(). La base e' spesso lontana: se una
// configurazione sbagliata la fa crashare (o bloccare) di continuo, nessuno
// puo' correggerla dal pannello. Dopo 3 avvii di fila finiti con un guasto
// prima di 10 minuti di funzionamento, ripristina l'ultima configurazione
// buona (/sdcard/config/settings_good.bin, scritta dopo 2 ore di
// funzionamento senza modifiche).
void config_backup_crash_guard(void);

// Ultimo ripristino fatto dalla protezione (per il pannello), "" se nessuno.
const char *config_backup_guard_note(void);

// Chiede la prima copia dopo l'avvio.
void config_backup_start(void);

// Scrive la copia se richiesta. Chiamata dal task del log diagnostico
// (diag_log.c) ogni 30 s, con la SD non montata.
void config_backup_service(void);
