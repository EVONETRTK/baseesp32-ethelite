#pragma once

#include <stdbool.h>

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"

// Fornisce i prossimi byte dell'immagine firmware da scrivere: deve
// copiare fino a max_len byte in buf e ritornare quanti ne ha
// effettivamente forniti, 0 se l'immagine e' terminata, <0 in caso di
// errore di lettura dalla fonte (upload HTTP, file su SD, ecc.).
typedef int (*ota_read_fn_t)(void *ctx, uint8_t *buf, size_t max_len);

// Scrive un'immagine firmware (letta a blocchi tramite read_cb, di
// qualunque provenienza) sulla partizione OTA non attiva e la imposta
// come partizione di avvio successivo. Non riavvia da sola: il chiamante
// decide quando farlo. Ritorna ESP_OK solo se l'intera immagine e' stata
// scritta e validata - in caso di errore la partizione di avvio corrente
// resta invariata, nessun rischio di avviare un'immagine incompleta.
esp_err_t ota_update_apply(ota_read_fn_t read_cb, void *ctx);

// Da chiamare una volta ad ogni avvio, dopo che il firmware si e'
// verificato funzionante (es. AP+web server avviati con successo):
// conferma l'immagine corrente al bootloader, disabilitando per questo
// avvio il rollback automatico. Se non viene mai chiamata e il firmware
// si blocca/riavvia in loop, il bootloader torna da solo all'immagine
// precedente funzionante (CONFIG_BOOTLOADER_APP_ROLLBACK_ENABLE).
void ota_update_mark_valid(void);

// Avvio: se il firmware e' appena stato installato lo lascia "in prova" e lo
// conferma dopo 5 minuti stabili (vedi ota_update.c). Sostituisce la
// conferma immediata.
void ota_update_start_confirm(void);
bool ota_update_is_pending(void);
void ota_update_confirm_tick(void); // dal task di sorveglianza, ogni 5 s

// Avvio: segnala (log e pannello) un ritorno automatico alla versione
// precedente avvenuto a questo avvio. Nota vuota se non e' successo.
void ota_update_check_rollback(void);
const char *ota_update_rollback_note(void);

// Confronto versioni "MAJOR.MINOR.PATCH": >0 se a>b, <0 se a<b, 0 se
// uguali. Ignora eventuali suffissi non numerici dopo il PATCH.
// Condiviso tra sd_update.c e online_update.c per non duplicarlo.
int ota_update_semver_compare(const char *a, const char *b);
