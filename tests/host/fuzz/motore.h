// Motore di fuzzing per le prove sul PC (vedi tests/host/fuzz.ps1).
//
// Ogni bersaglio (fuzz_*.c in questa cartella) include questo header, poi il
// .c vero del firmware, e definisce tre funzioni:
//   void bersaglio_avvio(void);   una volta all'inizio (init dei moduli)
//   void bersaglio_semi(void);    input validi di partenza: fuzz_seme(...)
//   void bersaglio_esegui(const uint8_t *dati, size_t n);   per ogni input
//
// Il motore genera milioni di input mutando i semi (byte cambiati, inseriti,
// cancellati, pezzi duplicati, valori limite, parole del dizionario, incroci
// tra input) e tiene quelli che fanno percorrere al firmware strade nuove
// (contatori dei blocchi messi dal compilatore: -fsanitize-coverage=
// inline-8bit-counters; trace-cmp per i valori confrontati).
// Per scoprire gli errori:
//   - ogni input sta in un buffer che finisce esattamente dove comincia una
//     pagina di memoria protetta (o, un input si' e uno no, che comincia
//     subito dopo): leggere o scrivere un byte oltre = crash immediato;
//   - malloc/calloc/realloc/free del firmware passano da qui: ogni blocco
//     finisce contro una pagina protetta e la memoria liberata non si riusa;
//   - comportamenti indefiniti (overflow di interi con segno, indici fuori
//     dagli array, conversioni di double fuori scala in int, shift...) sono
//     segnalati da UBSan con file e riga, e la ricerca continua;
//   - un input che dura piu' del tempo limite = ciclo infinito.
// Ogni errore salva l'input in fuzz/crash/ (.bin) con un riepilogo (.txt).
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>
#include <stdio.h>

// --- da definire nel bersaglio ------------------------------------------------
void bersaglio_avvio(void);
void bersaglio_semi(void);
void bersaglio_esegui(const uint8_t *dati, size_t n);

// --- servizi del motore ---------------------------------------------------------
// Input di partenza (copiati) e parole del dizionario per le mutazioni.
void fuzz_seme(const void *dati, size_t n);
void fuzz_seme_str(const char *s);
void fuzz_parola(const char *s);
void fuzz_parola_bin(const void *dati, size_t n);

// Buffer di n byte contro una pagina protetta (valido fino al prossimo input).
uint8_t *fuzz_buffer(size_t n);
// Copia di dati[0..n) come stringa C: il '\0' finale e' l'ultimo byte prima
// della pagina protetta (nei giri "allineati in fondo").
char *fuzz_stringa(const void *dati, size_t n);
// Le zone per i buffer sono poche (8): chi ne chiede molti per lo stesso
// input (es. un flusso a pezzi) le rende con questa (la copia dell'input resta).
void fuzz_zone_azzera(void);

// Numeri pseudo-casuali ripetibili per l'input in corso (stesso input =
// stessa sequenza): per spezzare un flusso in blocchi di misura varia.
uint32_t fuzz_caso(void);

// Impostazioni (da bersaglio_avvio): lunghezza massima degli input generati
// e tempo massimo per un input prima di dichiarare un ciclo infinito.
void fuzz_imposta_max_len(size_t n);
void fuzz_imposta_limite_ms(unsigned ms);

// Errore di coerenza trovato dal bersaglio stesso (es. una funzione dice di
// aver scritto una stringa piu' lunga del buffer): salvato come un crash,
// la ricerca continua.
void fuzz_segnala(const char *tipo, const char *dove);
#define FUZZ_VERIFICA(cond) \
    do { if (!(cond)) fuzz_segnala("controllo-fallito", __FILE__ ":" FUZZ_STR(__LINE__) ": " #cond); } while (0)
#define FUZZ_STR2(x) #x
#define FUZZ_STR(x) FUZZ_STR2(x)

// --- allocazioni protette ---------------------------------------------------------
void *fuzz_malloc(size_t n);
void *fuzz_calloc(size_t n, size_t size);
void *fuzz_realloc(void *p, size_t n);
void fuzz_free(void *p);
#ifndef FUZZ_MOTORE_INTERNO
#define malloc(n) fuzz_malloc(n)
#define calloc(n, s) fuzz_calloc((n), (s))
#define realloc(p, n) fuzz_realloc((p), (n))
#define free(p) fuzz_free(p)
#endif
