// Piccolo sistema di prove per il PC, senza librerie esterne.
//
// Uso in un file di prova:
//   PROVA(gga_rtk_fisso_letta) { VERIFICA_INT(q, 4); ... }
//   int main(void) { ESEGUI(gga_rtk_fisso_letta); return prove_fine(); }
//
// Una prova fallisce se almeno una verifica fallisce; le verifiche dopo la
// prima sbagliata continuano (si vedono tutti gli errori della prova).
// ESEGUI_BUG(nome, "spiegazione"): prova che mostra un bug noto del
// firmware, non ancora corretto. Se fallisce si conta tra i "bug noti" e non
// fa fallire il giro; se passa si avvisa di togliere il segno.
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <string.h>

#define PROVA(nome) static void nome(void)

void prove_esegui(const char *nome, void (*fn)(void), const char *bug);
int prove_fine(void);
void prove_errore(const char *file, int riga, const char *fmt, ...);

#define ESEGUI(nome) prove_esegui(#nome, nome, NULL)
#define ESEGUI_BUG(nome, spiegazione) prove_esegui(#nome, nome, spiegazione)

#define VERIFICA(cond) \
    do { if (!(cond)) prove_errore(__FILE__, __LINE__, "falso: %s", #cond); } while (0)

#define VERIFICA_INT(attuale, atteso) \
    do { long long a_ = (long long) (attuale), e_ = (long long) (atteso); \
         if (a_ != e_) prove_errore(__FILE__, __LINE__, "%s = %lld, atteso %lld", #attuale, a_, e_); } while (0)

#define VERIFICA_VICINO(attuale, atteso, toll) \
    do { double a_ = (double) (attuale), e_ = (double) (atteso), t_ = (double) (toll); \
         if (!(a_ - e_ <= t_ && e_ - a_ <= t_)) \
             prove_errore(__FILE__, __LINE__, "%s = %.12g, atteso %.12g (+/- %g, scarto %.3g)", \
                          #attuale, a_, e_, t_, a_ - e_); } while (0)

#define VERIFICA_STR(attuale, atteso) \
    do { const char *a_ = (attuale), *e_ = (atteso); \
         if (!a_ || strcmp(a_, e_) != 0) \
             prove_errore(__FILE__, __LINE__, "%s = \"%s\", atteso \"%s\"", #attuale, a_ ? a_ : "(NULL)", e_); } while (0)
