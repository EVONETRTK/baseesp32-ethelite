#pragma once

// Scrive su microSD (cartella /sdcard/diag_logs/) tutte le righe di log
// gia' raccolte da log_buffer.c - uno storico persistente oltre il buffer
// in RAM (8KB, perso al riavvio), utile per diagnosticare un problema
// successo quando nessuno era collegato alla pagina web nel momento
// giusto. Un file per sessione di avvio ("boot_0.log".."boot_4.log", in
// rotazione), tetto di circa 200KB a file.
//
// Non bloccante sul percorso di log (le righe vengono accodate su uno
// stream buffer interno, consumato da un task separato che scarica su SD
// periodicamente, non ad ogni riga) e monta la SD solo per la durata di
// ogni scarico, non la tiene occupata di continuo - per non bloccare le
// altre funzioni che la usano (aggiornamento da SD, registrazione PPP,
// archivio firmware).
//
// Va chiamata una sola volta all'avvio, dopo log_buffer_init().
void diag_log_start(void);
