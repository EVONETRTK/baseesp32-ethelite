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
