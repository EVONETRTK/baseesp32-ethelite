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
