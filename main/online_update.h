#pragma once

#include <stdbool.h>
#include <stddef.h>

// Scarica il manifest JSON all'URL configurato (settings.ota_update_url,
// tipicamente un asset di una release GitHub: {"version":"x.y.z",
// "url":"https://.../firmware.bin"}) e confronta la versione indicata con
// FIRMWARE_VERSION. Non scarica ne' applica il firmware stesso. out_version
// e out_url (se non NULL) ricevono i campi del manifest; out_msg (se non
// NULL) una descrizione dell'esito. Ritorna true solo se e' disponibile
// una versione piu' recente di quella corrente.
bool online_update_check(char *out_version, size_t out_version_size,
                          char *out_url, size_t out_url_size,
                          char *out_msg, size_t out_msg_size);

// Scarica e applica l'immagine a firmware_url (tipicamente quello
// restituito da online_update_check) via HTTPS. Non riavvia da sola: il
// chiamante decide quando farlo.
bool online_update_apply(const char *firmware_url, char *out_msg, size_t out_msg_size);
