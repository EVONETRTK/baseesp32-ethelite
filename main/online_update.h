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
// chiamante decide quando farlo. Bloccante: durante l'esecuzione aggiorna
// lo stato leggibile con online_update_get_progress().
bool online_update_apply(const char *firmware_url, char *out_msg, size_t out_msg_size);

typedef struct {
    bool running;      // aggiornamento in corso
    bool done;          // terminato (con successo o no)
    bool ok;             // valido solo se done == true
    int percent;        // 0-100, -1 se la dimensione totale non e' nota
    int bytes_read;
    int bytes_total;    // -1 se non nota (es. content-length assente)
    char message[96];
} online_update_progress_t;

// Avvia online_update_apply() in un task separato e ritorna subito - la UI
// web puo' cosi' interrogare l'avanzamento con online_update_get_progress()
// mentre il download e' in corso, invece di restare bloccata in attesa
// della risposta HTTP. Se l'aggiornamento riesce il dispositivo si riavvia
// da solo, come online_update_apply().
void online_update_apply_async(const char *firmware_url);

online_update_progress_t online_update_get_progress(void);
