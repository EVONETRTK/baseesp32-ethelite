// Esecuzione delle prove e riepilogo (vedi prove.h).
#include "prove.h"

#include <stdarg.h>
#include <stdio.h>

static int s_prove, s_fallite, s_bug_noti;
static int s_errori_prova;

void prove_errore(const char *file, int riga, const char *fmt, ...)
{
    s_errori_prova++;
    va_list ap;
    va_start(ap, fmt);
    printf("      %s:%d: ", file, riga);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

void prove_esegui(const char *nome, void (*fn)(void), const char *bug)
{
    s_errori_prova = 0;
    s_prove++;
    fn();
    if (s_errori_prova == 0) {
        if (bug) {
            printf("  OK      %s (segnata come bug noto ma ora passa: togliere ESEGUI_BUG)\n", nome);
        } else {
            printf("  OK      %s\n", nome);
        }
    } else if (bug) {
        s_bug_noti++;
        printf("  BUG     %s (nota: fallisce, bug noto: %s)\n", nome, bug);
    } else {
        s_fallite++;
        printf("  FALLITA %s\n", nome);
    }
    fflush(stdout);
}

int prove_fine(void)
{
    // Riga letta da run.ps1 per il riepilogo complessivo.
    printf("RIEPILOGO prove=%d fallite=%d bugnoti=%d\n", s_prove, s_fallite, s_bug_noti);
    fflush(stdout);
    return s_fallite ? 1 : 0;
}
