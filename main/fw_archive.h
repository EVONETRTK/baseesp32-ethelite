#pragma once

#include <stdbool.h>
#include <stddef.h>

// Archivia su microSD (cartella /sdcard/firmware/, un file per versione,
// es. "v1.15.0.bin") una copia del firmware ATTUALMENTE in esecuzione -
// da chiamare prima di sovrascriverlo con un aggiornamento, cosi' resta
// disponibile un ripristino manuale anche a distanza di piu' aggiornamenti
// (il rollback automatico del bootloader copre solo un passo indietro).
// Tiene solo le ultime 2 versioni, cancellando le piu' vecchie.
//
// Mai bloccante per l'aggiornamento in corso: se la SD non e' disponibile
// o e' gia' occupata da un'altra operazione (es. un aggiornamento
// lanciato proprio dalla SD, che la tiene gia' montata) si limita a non
// archiviare nulla per questa volta, senza errori propagati al chiamante.
void fw_archive_save_current(void);

typedef struct {
    char filename[32]; // es. "v1.15.0.bin", nome del file nella cartella archivio
} fw_archive_entry_t;

// Elenca i file nell'archivio, piu' recenti prima. Ritorna quante voci
// sono state scritte in out (fino a max_count).
size_t fw_archive_list(fw_archive_entry_t *out, size_t max_count);

// Applica un file specifico dall'archivio (stesso nome restituito da
// fw_archive_list) IGNORANDO il confronto versione - a differenza
// dell'aggiornamento normale da SD, che rifiuta un file non piu' recente.
// Pensato per un ripristino manuale deliberato, mai automatico. Non
// riavvia da sola: il chiamante decide quando farlo.
bool fw_archive_apply(const char *filename, char *out_msg, size_t out_msg_size);
