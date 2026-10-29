// Prove di main/pending_restart.c: impostazioni salvate che valgono solo
// dopo un riavvio (barra "Riavvia" del pannello).
#include "prove.h"
#include "../../main/pending_restart.c"

#include <string.h>

// Revisione del 29/10/2026: i messaggi RTCM della base si impostano nel
// ricevitore solo all'avvio, ma un cambio dal pannello non chiedeva il
// riavvio e la base continuava con quelli di prima.
PROVA(messaggi_rtcm_chiedono_il_riavvio)
{
    static app_settings_t s;
    memset(&s, 0, sizeof(s));
    s.rtcm_1005_enable = true;
    s.rtcm_1074_enable = true;
    pending_restart_snapshot(&s);

    char out[256];
    pending_restart_describe(&s, out, sizeof(out));
    VERIFICA(out[0] == '\0');

    s.rtcm_1074_enable = false; // da MSM4 a MSM7 per il GPS
    s.rtcm_1077_enable = true;
    pending_restart_describe(&s, out, sizeof(out));
    VERIFICA(strstr(out, "messaggi RTCM della base") != NULL);

    s.rtcm_1074_enable = true; // rimessi come all'avvio: niente riavvio
    s.rtcm_1077_enable = false;
    pending_restart_describe(&s, out, sizeof(out));
    VERIFICA(out[0] == '\0');

    s.rtcm_1127_enable = true; // ultimo della lista
    pending_restart_describe(&s, out, sizeof(out));
    VERIFICA(strstr(out, "messaggi RTCM della base") != NULL);
}

int main(void)
{
    ESEGUI(messaggi_rtcm_chiedono_il_riavvio);
    return prove_fine();
}
