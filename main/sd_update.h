#pragma once

#include <stdbool.h>
#include <stddef.h>

// Monta la scheda microSD (lettore integrato, pin fissi da Kconfig),
// cerca firmware.bin + firmware.json (con il campo "version") nella
// radice; se la versione indicata e' piu' recente di FIRMWARE_VERSION
// applica l'aggiornamento (stesso core di ota_update.h) e rinomina il
// file applicato in firmware.bin.applied per non riflasharlo di nuovo.
// Non riavvia da sola: il chiamante decide quando farlo. Ritorna true
// solo se un aggiornamento e' stato applicato con successo. out_msg (se
// non NULL) riceve una breve descrizione dell'esito in ogni caso, utile
// per la UI web - non richiede connettivita' di rete.
bool sd_update_check_and_apply(char *out_msg, size_t out_msg_size);

typedef struct {
    bool checked;       // true dal primo sd_update_check_and_apply() in poi (boot o pulsante manuale)
    bool card_present;  // valido solo se checked == true
    char message[96];   // stesso testo restituito da sd_update_check_and_apply()
} sd_update_status_t;

// Esito dell'ultimo sd_update_check_and_apply() (boot automatico o
// pulsante manuale nella UI web) - utile per mostrare nella scheda Stato
// se la scheda SD e' stata rilevata, senza dover guardare il log seriale.
sd_update_status_t sd_update_get_status(void);
