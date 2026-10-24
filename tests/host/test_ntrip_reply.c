// Prove di main/ntrip_reply.h: spiegazione dei rifiuti del caster e quali
// rifiuti escludono una base scelta in automatico.
#include "prove.h"
#include "../../main/ntrip_reply.h"

// Prova del 24/10/2026 (riavvii ravvicinati): "troppe sessioni" e' un
// rifiuto passeggero, non deve escludere la base.
PROVA(rifiuti_che_escludono_la_base)
{
    VERIFICA(!ntrip_reply_refuses_mountpoint("HTTP/1.1 401 Unauthorized")); // account, non base
    VERIFICA(ntrip_reply_refuses_mountpoint("HTTP/1.0 403 Forbidden"));
    VERIFICA(ntrip_reply_refuses_mountpoint("HTTP/1.1 404 Not Found"));
    VERIFICA(ntrip_reply_refuses_mountpoint("SOURCETABLE 200 OK"));
    VERIFICA(!ntrip_reply_refuses_mountpoint("HTTP/1.1 429 Too Many Requests"));
    VERIFICA(!ntrip_reply_refuses_mountpoint("HTTP/1.1 402 Payment Required"));
    VERIFICA(!ntrip_reply_refuses_mountpoint("HTTP/1.1 500 Internal Server Error"));
    VERIFICA(!ntrip_reply_refuses_mountpoint("ERROR - qualcosa"));
    VERIFICA(!ntrip_reply_refuses_mountpoint(""));
    VERIFICA(!ntrip_reply_refuses_mountpoint("HTTP/1.1"));
    // NTRIP 1 (revisione del 24/10/2026: prima non escludeva piu' la base)
    VERIFICA(ntrip_reply_refuses_mountpoint("ERROR - Bad Mountpoint"));
    VERIFICA(ntrip_reply_refuses_mountpoint("ERROR - unknown MOUNT POINT"));
    VERIFICA(!ntrip_reply_refuses_mountpoint("ERROR - mount"));
}

PROVA(rifiuti_per_l_account)
{
    VERIFICA(ntrip_reply_account_level("HTTP/1.1 429 Too Many Requests"));
    VERIFICA(ntrip_reply_account_level("HTTP/1.1 402 Payment Required"));
    VERIFICA(!ntrip_reply_account_level("HTTP/1.1 503 Service Unavailable"));
    // Password sbagliata: vale per tutte le basi, nessuna esclusione (revisione
    // del 24/10/2026: escludeva le basi una dopo l'altra per sempre).
    VERIFICA(ntrip_reply_account_level("HTTP/1.1 401 Unauthorized"));
    VERIFICA(!ntrip_reply_account_level(""));
    // Nomina la mountpoint ma e' un rifiuto per l'account: il rover controlla
    // prima questo e la base non viene esclusa.
    VERIFICA(ntrip_reply_account_level("HTTP/1.1 429 too many sessions on mountpoint X"));
}

PROVA(spiegazione_dei_rifiuti)
{
    VERIFICA(strstr(ntrip_explain_reply("HTTP/1.1 429 Too Many Requests", false), "troppe sessioni") != NULL);
    VERIFICA(strstr(ntrip_explain_reply("HTTP/1.1 401 Unauthorized", false), "password") != NULL);
    VERIFICA(ntrip_explain_reply("HTTP/1.1 418 I'm a teapot", false) == NULL);
}

int main(void)
{
    ESEGUI(rifiuti_che_escludono_la_base);
    ESEGUI(rifiuti_per_l_account);
    ESEGUI(spiegazione_dei_rifiuti);
    return prove_fine();
}
