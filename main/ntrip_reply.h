#pragma once

#include <stdbool.h>
#include <string.h>

// Traduce la prima riga di un rifiuto del caster NTRIP in una frase chiara
// per il pannello e i log, invece della risposta grezza del protocollo
// (es. "SOURCETABLE 200 OK" che per una base significa "mountpoint
// sconosciuta" - capito solo leggendo il codice del caster EVONETRTK).
// Ritorna NULL se la risposta non e' tra quelle note: il chiamante mostra
// allora la risposta grezza.
static inline const char *ntrip_explain_reply(const char *resp, bool as_source)
{
    if (strncmp(resp, "SOURCETABLE", 11) == 0 || strstr(resp, " 404") != NULL) {
        return as_source
            ? "mountpoint sconosciuta al caster: non esiste tra le basi configurate sul caster (controlla nome, maiuscole comprese, e che la base sia attiva)"
            : "mountpoint sconosciuta o al momento senza base che trasmette";
    }
    if (strstr(resp, "401") != NULL || strstr(resp, "Unauthorized") != NULL || strstr(resp, "Bad Password") != NULL) {
        return as_source
            ? "password sorgente non accettata dal caster per questa mountpoint"
            : "utente o password non accettati dal caster";
    }
    if (strstr(resp, "409") != NULL || strstr(resp, "Taken") != NULL || strstr(resp, "in use") != NULL ||
        strstr(resp, "in uso") != NULL) {
        return "mountpoint gia' occupata da un'altra base collegata (o da questa stessa, gia' connessa)";
    }
    if (strstr(resp, "402") != NULL) {
        return "abbonamento non attivo o scaduto";
    }
    if (strstr(resp, "429") != NULL) {
        return "troppe sessioni contemporanee per questo abbonamento";
    }
    return NULL;
}

// Rifiuto che riguarda la mountpoint (403 accesso negato, 404 o tabella delle
// sorgenti = mountpoint inesistente, "Bad Mountpoint"): solo questo esclude per 1-4 ore una base scelta in automatico
// (ntrip_rover_client.c). "Troppe sessioni contemporanee" (429: la sessione
// di prima di un riavvio ancora aperta sul caster), abbonamento scaduto (402)
// e altro non dipendono dalla base: escluderla lasciava il rover su una base
// peggiore (prova del 24/10/2026). resp = prima riga della risposta.
static inline bool ntrip_reply_refuses_mountpoint(const char *resp)
{
    if (strncmp(resp, "SOURCETABLE", 11) == 0) {
        return true;
    }
    const char *sp = strchr(resp, ' ');
    if (sp && (strncmp(sp + 1, "403", 3) == 0 || strncmp(sp + 1, "404", 3) == 0)) {
        return true;
    }
    // NTRIP 1: "ERROR - Bad Mountpoint" e simili (maiuscole o no).
    for (const char *p = resp; *p; p++) {
        static const char *const w[] = { "mountpoint", "mount point" };
        for (int k = 0; k < 2; k++) {
            size_t i = 0;
            while (w[k][i] && p[i] && (p[i] | 0x20) == w[k][i]) {
                i++;
            }
            if (w[k][i] == '\0') {
                return true;
            }
        }
    }
    return false;
}

// Rifiuto che dipende dall'account e non dalla base (401 utente o password,
// 429 troppe sessioni, 402 abbonamento): mai contato contro la base scelta.
// 401 qui e non tra i rifiuti della mountpoint: con una password sbagliata il
// rover escludeva le basi una dopo l'altra, con un download della tabella e un
// accesso sbagliato ogni 2 minuti per sempre (revisione del 24/10/2026).
static inline bool ntrip_reply_account_level(const char *resp)
{
    const char *sp = strchr(resp, ' ');
    return sp && (strncmp(sp + 1, "401", 3) == 0 || strncmp(sp + 1, "429", 3) == 0 || strncmp(sp + 1, "402", 3) == 0);
}
