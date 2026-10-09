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

// --- comportamenti indefiniti "di valore" (vedi run.ps1) ---------------------
// Conversioni di double fuori scala in int, overflow di interi con segno e
// shift fuori misura non fermano il programma di prova (run.ps1 li compila
// senza trap): fanno fallire la prova in corso con file e riga, cosi' una
// prova di regressione di un bug noto (ESEGUI_BUG) li puo' mostrare senza
// interrompere le altre. Indici fuori dagli array, puntatori nulli o
// disallineati restano con trap (programma fermato: "INTERROTTA").
typedef struct { const char *file; uint32_t riga, col; } ubsan_pos_t;

static void ub_valore(const char *tipo, const void *dati)
{
    const ubsan_pos_t *p = (const ubsan_pos_t *) dati;
    const char *f = p && p->file ? p->file : "?";
    const char *b = strstr(f, "main");
    prove_errore(b ? b : f, p ? (int) p->riga : 0, "comportamento indefinito: %s", tipo);
}

void __ubsan_handle_float_cast_overflow(void *d, void *a) { (void) a; ub_valore("double fuori scala convertito in intero", d); }
void __ubsan_handle_add_overflow(void *d, void *a, void *b) { (void) a; (void) b; ub_valore("overflow di un intero con segno (+)", d); }
void __ubsan_handle_sub_overflow(void *d, void *a, void *b) { (void) a; (void) b; ub_valore("overflow di un intero con segno (-)", d); }
void __ubsan_handle_mul_overflow(void *d, void *a, void *b) { (void) a; (void) b; ub_valore("overflow di un intero con segno (*)", d); }
void __ubsan_handle_negate_overflow(void *d, void *a) { (void) a; ub_valore("overflow di un intero con segno (cambio di segno)", d); }
void __ubsan_handle_divrem_overflow(void *d, void *a, void *b) { (void) a; (void) b; ub_valore("divisione intera in overflow o per zero", d); }
void __ubsan_handle_shift_out_of_bounds(void *d, void *a, void *b) { (void) a; (void) b; ub_valore("shift fuori misura", d); }
