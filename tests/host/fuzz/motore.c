// Motore di fuzzing (vedi motore.h). Solo Windows (VirtualAlloc, dbghelp).
// Questo file NON e' strumentato: niente copertura e niente UBSan qui dentro.
#define FUZZ_MOTORE_INTERNO
#include "motore.h"
// fuzz.ps1 include motore.h in ogni file (-include) prima di questa riga:
// qui servono le allocazioni vere.
#undef malloc
#undef calloc
#undef realloc
#undef free

#include <windows.h>

#include <signal.h>
#include <stdarg.h>

#pragma clang attribute push(__attribute__((no_sanitize("coverage", "undefined"))), apply_to = function)

// =============================================================================
// Stato generale
static const char *g_nome = "bersaglio";
static const char *g_dir_crash = ".";
static uint64_t g_seme = 1;
static size_t g_max_len = 4096;
static unsigned g_limite_ms = 2000;
static volatile LONG64 g_input_provati;
static volatile LONG g_in_esecuzione;
static volatile ULONGLONG g_inizio_input_ms;
static HANDLE g_thread_principale;
static int g_errori_trovati;
static bool g_riproduzione;
static bool g_davanti; // riproduzione: buffer subito dopo la pagina protetta

// Input in esecuzione (copia, per salvarlo in caso di crash).
static uint8_t *g_corrente;
static size_t g_corrente_len;

// =============================================================================
// Numeri casuali (xorshift64*): uno per le mutazioni (dal seme) e uno per
// l'input in corso (dall'hash dell'input, cosi' e' ripetibile).
static uint64_t g_rng = 88172645463325252ull, g_rng_input = 1;

static uint64_t xs64(uint64_t *s)
{
    uint64_t x = *s;
    x ^= x >> 12;
    x ^= x << 25;
    x ^= x >> 27;
    *s = x;
    return x * 2685821657736338717ull;
}

static uint32_t rnd(void) { return (uint32_t) (xs64(&g_rng) >> 32); }
static uint32_t rnd_n(uint32_t n) { return n ? rnd() % n : 0; }

uint32_t fuzz_caso(void) { return (uint32_t) (xs64(&g_rng_input) >> 32); }

static uint64_t hash_dati(const uint8_t *d, size_t n)
{
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; i++) {
        h = (h ^ d[i]) * 1099511628211ull;
    }
    return h;
}

// =============================================================================
// Copertura (-fsanitize-coverage=inline-8bit-counters,trace-cmp): il
// compilatore mette un contatore da un byte per ogni blocco di codice del
// firmware nella sezione .SCOV$CM; questi due segnaposto la delimitano
// (il linker ordina le sezioni per nome: CA < CM < CZ).
__attribute__((section(".SCOV$CA"))) uint64_t __start___sancov_cntrs = 0;
__attribute__((section(".SCOV$CZ"))) uint64_t __stop___sancov_cntrs = 0;
static uint8_t *g_mappa;   // contatori dei blocchi (dal compilatore)
static size_t g_mappa_n;
static uint8_t *g_vista;   // per ogni blocco: classi di conteggio gia' viste (bit)
static bool g_diagnosi;

void __sanitizer_cov_8bit_counters_init(uint8_t *inizio, uint8_t *fine)
{
    // chiamata da ogni file strumentato, sempre con la stessa sezione intera
    if (g_mappa) return;
    g_mappa = inizio;
    g_mappa_n = (size_t) (fine - inizio);
    g_vista = calloc(g_mappa_n ? g_mappa_n : 1, 1);
}

// Valori costanti confrontati dal firmware (es. 0xD3, ',' , 1005): usati
// dalle mutazioni per indovinare i valori "magici".
#define TORC 512
static struct { uint64_t v; uint8_t size; } g_torc[TORC];
static unsigned g_torc_n;

static void torc_metti(uint64_t v, uint8_t size)
{
    unsigned i = (unsigned) ((v * 0x9E3779B97F4A7C15ull) >> 55) % TORC; // stessa costante, stesso posto
    g_torc[i].v = v;
    g_torc[i].size = size;
    if (g_torc_n < TORC) g_torc_n++;
}

void __sanitizer_cov_trace_const_cmp1(uint8_t a, uint8_t b) { (void) b; torc_metti(a, 1); }
void __sanitizer_cov_trace_const_cmp2(uint16_t a, uint16_t b) { (void) b; torc_metti(a, 2); }
void __sanitizer_cov_trace_const_cmp4(uint32_t a, uint32_t b) { (void) b; torc_metti(a, 4); }
void __sanitizer_cov_trace_const_cmp8(uint64_t a, uint64_t b) { (void) b; torc_metti(a, 8); }
void __sanitizer_cov_trace_cmp1(uint8_t a, uint8_t b) { (void) a; (void) b; }
void __sanitizer_cov_trace_cmp2(uint16_t a, uint16_t b) { (void) a; (void) b; }
void __sanitizer_cov_trace_cmp4(uint32_t a, uint32_t b) { (void) a; (void) b; }
void __sanitizer_cov_trace_cmp8(uint64_t a, uint64_t b) { (void) a; (void) b; }
void __sanitizer_cov_trace_switch(uint64_t v, uint64_t *casi)
{
    (void) v;
    // casi[0] = numero di casi, casi[1] = bit, poi i valori
    for (uint64_t i = 0; i < casi[0] && i < 64; i++) {
        torc_metti(casi[2 + i], (uint8_t) (casi[1] / 8));
    }
}

static uint8_t classe(uint8_t c)
{
    if (c == 0) return 0;
    if (c == 1) return 1;
    if (c == 2) return 2;
    if (c == 3) return 4;
    if (c <= 7) return 8;
    if (c <= 15) return 16;
    if (c <= 31) return 32;
    if (c <= 127) return 64;
    return 128;
}

// true se l'ultimo input ha percorso strade nuove (e le registra).
static bool copertura_nuova(void)
{
    bool nuova = false;
    for (size_t i = 0; i < g_mappa_n; i++) {
        if (!g_mappa[i]) continue;
        uint8_t c = classe(g_mappa[i]);
        if (!(g_vista[i] & c)) {
            g_vista[i] |= c;
            nuova = true;
        }
    }
    return nuova;
}

// Blocchi di codice percorsi almeno una volta (su quanti strumentati).
static unsigned caselle_viste(void)
{
    unsigned n = 0;
    for (size_t i = 0; i < g_mappa_n; i++) n += g_vista[i] != 0;
    return n;
}

// =============================================================================
// Memoria protetta
static size_t g_pagina = 4096;

// Zone per gli input e i buffer dei bersagli: [protetta][dati][protetta].
#define ZONE 8
#define ZONA_DATI (1u << 20) // 1 MB
static uint8_t *g_zona[ZONE];
static int g_zona_usate;
static bool g_allinea_fine = true;

static void zone_crea(void)
{
    for (int i = 0; i < ZONE; i++) {
        size_t tot = ZONA_DATI + 2 * g_pagina;
        uint8_t *b = VirtualAlloc(NULL, tot, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
        if (!b) {
            fprintf(stderr, "VirtualAlloc fallita\n");
            exit(2);
        }
        DWORD old;
        VirtualProtect(b, g_pagina, PAGE_NOACCESS, &old);
        VirtualProtect(b + g_pagina + ZONA_DATI, g_pagina, PAGE_NOACCESS, &old);
        g_zona[i] = b + g_pagina;
    }
}

uint8_t *fuzz_buffer(size_t n)
{
    if (n > ZONA_DATI || g_zona_usate >= ZONE) {
        fprintf(stderr, "fuzz_buffer: troppi buffer o troppo grandi (%zu)\n", n);
        exit(2);
    }
    uint8_t *z = g_zona[g_zona_usate++];
    return g_allinea_fine ? z + ZONA_DATI - n : z;
}

void fuzz_zone_azzera(void)
{
    g_zona_usate = 1;
}

char *fuzz_stringa(const void *dati, size_t n)
{
    char *s = (char *) fuzz_buffer(n + 1);
    memcpy(s, dati, n);
    s[n] = '\0';
    return s;
}

// malloc del firmware: blocco a fine pagina contro una pagina protetta;
// intestazione all'inizio della prenotazione per ritrovare la dimensione.
#define MAGICO 0x5A5A4655u
typedef struct { uint32_t magico; size_t n; } intest_t;

void *fuzz_malloc(size_t n)
{
    size_t dati = (n + sizeof(intest_t) + g_pagina - 1) / g_pagina * g_pagina;
    uint8_t *b = VirtualAlloc(NULL, dati + g_pagina, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE);
    if (!b) return NULL;
    DWORD old;
    VirtualProtect(b + dati, g_pagina, PAGE_NOACCESS, &old);
    intest_t *h = (intest_t *) b;
    h->magico = MAGICO;
    h->n = n;
    // Allineamento: la potenza di 2 piu' grande che divide n (max 16), cosi'
    // un array di strutture finisce esattamente contro la pagina protetta.
    size_t al = 16;
    while (al > 1 && (n % al)) al >>= 1;
    uintptr_t p = ((uintptr_t) (b + dati) - n) & ~(uintptr_t) (al - 1);
    memset((void *) p, 0xA5, n); // contenuto iniziale non nullo, come una malloc vera
    return (void *) p;
}

static intest_t *intest_di(void *p)
{
    MEMORY_BASIC_INFORMATION mi;
    if (!p || !VirtualQuery(p, &mi, sizeof(mi)) || mi.State != MEM_COMMIT) return NULL;
    intest_t *h = (intest_t *) mi.AllocationBase;
    if (!h || !VirtualQuery(h, &mi, sizeof(mi)) || mi.Protect != PAGE_READWRITE || h->magico != MAGICO) return NULL;
    return h;
}

void *fuzz_calloc(size_t n, size_t size)
{
    if (size && n > SIZE_MAX / size) return NULL;
    void *p = fuzz_malloc(n * size);
    if (p) memset(p, 0, n * size);
    return p;
}

void fuzz_free(void *p)
{
    if (!p) return;
    intest_t *h = intest_di(p);
    if (!h) {
        free(p); // non nostro (es. allocato dalla libreria C)
        return;
    }
    // La memoria liberata resta prenotata e diventa inaccessibile: un uso
    // dopo free() fa crash subito. Rilasciata solo l'area dei dati.
    h->magico = 0;
    MEMORY_BASIC_INFORMATION mi;
    VirtualQuery(h, &mi, sizeof(mi));
    VirtualFree(h, 0, MEM_RELEASE);
}

void *fuzz_realloc(void *p, size_t n)
{
    if (!p) return fuzz_malloc(n);
    intest_t *h = intest_di(p);
    if (!h) return realloc(p, n);
    void *q = fuzz_malloc(n);
    if (q) {
        memcpy(q, p, h->n < n ? h->n : n);
        fuzz_free(p);
    }
    return q;
}

// =============================================================================
// Corpus, dizionario, semi
typedef struct { uint8_t *d; size_t n; } voce_t;
#define CORPUS_MAX 8192
static voce_t g_corpus[CORPUS_MAX];
static size_t g_corpus_n;
#define DIZ_MAX 512
static voce_t g_diz[DIZ_MAX];
static size_t g_diz_n;
static voce_t g_semi[CORPUS_MAX];
static size_t g_semi_n;

static voce_t copia_voce(const void *d, size_t n)
{
    voce_t v = { malloc(n ? n : 1), n };
    memcpy(v.d, d, n);
    return v;
}

void fuzz_seme(const void *dati, size_t n)
{
    if (g_semi_n < CORPUS_MAX) g_semi[g_semi_n++] = copia_voce(dati, n);
}

void fuzz_seme_str(const char *s) { fuzz_seme(s, strlen(s)); }

void fuzz_parola_bin(const void *dati, size_t n)
{
    if (g_diz_n < DIZ_MAX && n) g_diz[g_diz_n++] = copia_voce(dati, n);
}

void fuzz_parola(const char *s) { fuzz_parola_bin(s, strlen(s)); }

void fuzz_imposta_max_len(size_t n) { g_max_len = n; }
void fuzz_imposta_limite_ms(unsigned ms) { g_limite_ms = ms; }

static void corpus_aggiungi(const uint8_t *d, size_t n)
{
    if (g_corpus_n < CORPUS_MAX) {
        g_corpus[g_corpus_n++] = copia_voce(d, n);
    } else {
        size_t i = rnd_n(CORPUS_MAX);
        free(g_corpus[i].d);
        g_corpus[i] = copia_voce(d, n);
    }
}

// =============================================================================
// Rapporto degli errori: stack con file e riga (dbghelp + PDB di zig)
static bool g_sym_ok;
// Dichiarazioni minime di dbghelp (zig non ha dbghelp.h ne' la libreria).
typedef enum { AddrMode1616, AddrMode1632, AddrModeReal, AddrModeFlat } ADDRESS_MODE;
typedef struct { DWORD64 Offset; WORD Segment; ADDRESS_MODE Mode; } ADDRESS64;
typedef struct {
    ADDRESS64 AddrPC, AddrReturn, AddrFrame, AddrStack, AddrBStore;
    PVOID FuncTableEntry;
    DWORD64 Params[4];
    BOOL Far, Virtual;
    DWORD64 Reserved[3];
    uint8_t KdHelp[256]; // KDHELP64 (con margine)
} STACKFRAME64, *LPSTACKFRAME64;
typedef struct {
    ULONG SizeOfStruct, TypeIndex;
    ULONG64 Reserved[2];
    ULONG Index, Size;
    ULONG64 ModBase;
    ULONG Flags;
    ULONG64 Value, Address;
    ULONG Register, Scope, Tag, NameLen, MaxNameLen;
    CHAR Name[1];
} SYMBOL_INFO, *PSYMBOL_INFO;
typedef struct { DWORD SizeOfStruct; PVOID Key; DWORD LineNumber; PCHAR FileName; DWORD64 Address; } IMAGEHLP_LINE64,
    *PIMAGEHLP_LINE64;
typedef PVOID (WINAPI *PFUNCTION_TABLE_ACCESS_ROUTINE64)(HANDLE, DWORD64);
typedef DWORD64 (WINAPI *PGET_MODULE_BASE_ROUTINE64)(HANDLE, DWORD64);
typedef PVOID PREAD_PROCESS_MEMORY_ROUTINE64, PTRANSLATE_ADDRESS_ROUTINE64;
#define SYMOPT_UNDNAME 0x2
#define SYMOPT_DEFERRED_LOADS 0x4
#define SYMOPT_LOAD_LINES 0x10

// dbghelp caricata a run-time (zig non ha la libreria di importazione).
typedef DWORD (WINAPI *p_SymSetOptions)(DWORD);
typedef BOOL (WINAPI *p_SymInitialize)(HANDLE, PCSTR, BOOL);
typedef BOOL (WINAPI *p_StackWalk64)(DWORD, HANDLE, HANDLE, LPSTACKFRAME64, PVOID, PREAD_PROCESS_MEMORY_ROUTINE64,
                                     PFUNCTION_TABLE_ACCESS_ROUTINE64, PGET_MODULE_BASE_ROUTINE64,
                                     PTRANSLATE_ADDRESS_ROUTINE64);
typedef BOOL (WINAPI *p_SymFromAddr)(HANDLE, DWORD64, PDWORD64, PSYMBOL_INFO);
typedef BOOL (WINAPI *p_SymGetLineFromAddr64)(HANDLE, DWORD64, PDWORD, PIMAGEHLP_LINE64);
static p_StackWalk64 pStackWalk64;
static p_SymFromAddr pSymFromAddr;
static p_SymGetLineFromAddr64 pSymGetLineFromAddr64;
static PFUNCTION_TABLE_ACCESS_ROUTINE64 pSymFunctionTableAccess64;
static PGET_MODULE_BASE_ROUTINE64 pSymGetModuleBase64;

static bool carica_dbghelp(void)
{
    HMODULE m = LoadLibraryA("dbghelp.dll");
    if (!m) return false;
    p_SymSetOptions so = (p_SymSetOptions) (void *) GetProcAddress(m, "SymSetOptions");
    p_SymInitialize si = (p_SymInitialize) (void *) GetProcAddress(m, "SymInitialize");
    pStackWalk64 = (p_StackWalk64) (void *) GetProcAddress(m, "StackWalk64");
    pSymFromAddr = (p_SymFromAddr) (void *) GetProcAddress(m, "SymFromAddr");
    pSymGetLineFromAddr64 = (p_SymGetLineFromAddr64) (void *) GetProcAddress(m, "SymGetLineFromAddr64");
    pSymFunctionTableAccess64 = (PFUNCTION_TABLE_ACCESS_ROUTINE64) (void *) GetProcAddress(m, "SymFunctionTableAccess64");
    pSymGetModuleBase64 = (PGET_MODULE_BASE_ROUTINE64) (void *) GetProcAddress(m, "SymGetModuleBase64");
    if (!so || !si || !pStackWalk64 || !pSymFromAddr || !pSymGetLineFromAddr64 || !pSymFunctionTableAccess64 ||
        !pSymGetModuleBase64) {
        return false;
    }
    so(SYMOPT_LOAD_LINES | SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    // Il PDB sta accanto all'eseguibile (build\fuzz): va detto a dbghelp.
    char dir[MAX_PATH];
    DWORD l = GetModuleFileNameA(NULL, dir, sizeof(dir));
    while (l > 0 && dir[l - 1] != 0x5C) l--; // barra rovesciata
    dir[l ? l - 1 : 0] = '\0';
    return si(GetCurrentProcess(), dir, TRUE);
}

static CRITICAL_SECTION g_cs_rapporto;

static bool in_motore(const char *file)
{
    return file && (strstr(file, "motore.c") || strstr(file, "host_rt.c"));
}

// Scrive in out le righe dello stack; in firma "funzione file:riga" del
// primo frame del firmware (main\) o, in mancanza, del bersaglio.
static void stack_testo(HANDLE thread, CONTEXT *ctx_in, char *out, size_t out_size, char *firma, size_t firma_size)
{
    out[0] = '\0';
    firma[0] = '\0';
    if (!g_sym_ok) {
        snprintf(out, out_size, "  (simboli non disponibili)\n");
        return;
    }
    // Risalita dello stack con le tabelle di unwind (.pdata) del codice x64.
    CONTEXT ctx = *ctx_in;
    (void) thread;
    HANDLE proc = GetCurrentProcess();
    char firma_bersaglio[256] = "";
    size_t k = 0;
    for (int i = 0; i < 24 && ctx.Rip; i++) {
        DWORD64 pc = ctx.Rip, pc_riga = i ? pc - 1 : pc; // dopo il primo: indirizzo di ritorno
        DWORD64 base_img = 0;
        PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &base_img, NULL);
        if (fe) {
            PVOID hd = NULL;
            DWORD64 ef = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base_img, pc, fe, &ctx, &hd, &ef, NULL);
        } else if (!IsBadReadPtr((void *) (uintptr_t) ctx.Rsp, 8)) {
            ctx.Rip = *(DWORD64 *) (uintptr_t) ctx.Rsp; // funzione foglia
            ctx.Rsp += 8;
        } else {
            ctx.Rip = 0;
        }
        DWORD64 addr = pc_riga;
        char symbuf[sizeof(SYMBOL_INFO) + 256];
        SYMBOL_INFO *sym = (SYMBOL_INFO *) symbuf;
        memset(symbuf, 0, sizeof(symbuf));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char *fn = pSymFromAddr(proc, addr, &disp, sym) ? sym->Name : "?";
        IMAGEHLP_LINE64 line;
        memset(&line, 0, sizeof(line));
        line.SizeOfStruct = sizeof(line);
        DWORD d2 = 0;
        bool ha_riga = pSymGetLineFromAddr64(proc, addr, &d2, &line);
        char riga[512];
        if (ha_riga) {
            snprintf(riga, sizeof(riga), "  %s  %s:%lu\n", fn, line.FileName, (unsigned long) line.LineNumber);
        } else {
            snprintf(riga, sizeof(riga), "  %s  (0x%llx)\n", fn, (unsigned long long) pc);
        }
        size_t l = strlen(riga);
        if (k + l < out_size) {
            memcpy(out + k, riga, l + 1);
            k += l;
        }
        if (ha_riga && !in_motore(line.FileName)) {
            const char *base = strrchr(line.FileName, '\\');
            base = base ? base + 1 : line.FileName;
            bool firmware = strstr(line.FileName, "\\main\\") || strstr(line.FileName, "/main/");
            if (firmware && !firma[0]) {
                snprintf(firma, firma_size, "%s %s:%lu", fn, base, (unsigned long) line.LineNumber);
            } else if (!firma_bersaglio[0]) {
                snprintf(firma_bersaglio, sizeof(firma_bersaglio), "%s %s:%lu", fn, base, (unsigned long) line.LineNumber);
            }
        }
    }
    if (!firma[0]) {
        snprintf(firma, firma_size, "%s", firma_bersaglio[0] ? firma_bersaglio : "sconosciuta");
    }
}

static void pulisci_nome(char *s)
{
    for (; *s; s++) {
        if (!((*s >= 'a' && *s <= 'z') || (*s >= 'A' && *s <= 'Z') || (*s >= '0' && *s <= '9') || *s == '-' || *s == '_')) {
            *s = '_';
        }
    }
}

// Salva l'input corrente e il riepilogo; stampa la riga ERRORE letta da fuzz.ps1.
static void rapporto(const char *tipo, const char *firma, const char *dettagli, const char *stack)
{
    EnterCriticalSection(&g_cs_rapporto);
    g_errori_trovati++;
    uint64_t h = hash_dati(g_corrente, g_corrente_len);
    char tipo_file[64];
    snprintf(tipo_file, sizeof(tipo_file), "%s", tipo);
    pulisci_nome(tipo_file);
    char base[MAX_PATH];
    snprintf(base, sizeof(base), "%s\\%s-%s-%08x", g_dir_crash, g_nome, tipo_file, (unsigned) h);
    char percorso[MAX_PATH + 8];
    if (!g_riproduzione) {
        snprintf(percorso, sizeof(percorso), "%s.bin", base);
        FILE *f = fopen(percorso, "wb");
        if (f) {
            fwrite(g_corrente, 1, g_corrente_len, f);
            fclose(f);
        }
        snprintf(percorso, sizeof(percorso), "%s.txt", base);
        f = fopen(percorso, "w");
        if (f) {
            fprintf(f, "Bersaglio: %s\nTipo: %s\nFirma: %s\nDettagli: %s\nInput: %zu byte (%s.bin), seme %llu, input n. %lld, "
                       "buffer contro la pagina protetta %s\n",
                    g_nome, tipo, firma, dettagli, g_corrente_len, base, (unsigned long long) g_seme,
                    (long long) g_input_provati, g_allinea_fine ? "in fondo" : "all'inizio");
            fprintf(f, "Stack:\n%s", stack ? stack : "  (non disponibile)\n");
            fprintf(f, "Input (primi 512 byte, esadecimale e testo):\n");
            for (size_t i = 0; i < g_corrente_len && i < 512; i += 16) {
                fprintf(f, "  %04zx  ", i);
                for (size_t j = i; j < i + 16; j++) {
                    if (j < g_corrente_len) fprintf(f, "%02x ", g_corrente[j]);
                    else fprintf(f, "   ");
                }
                fprintf(f, " ");
                for (size_t j = i; j < i + 16 && j < g_corrente_len; j++) {
                    fputc(g_corrente[j] >= 0x20 && g_corrente[j] < 0x7f ? g_corrente[j] : '.', f);
                }
                fprintf(f, "\n");
            }
            fclose(f);
        }
    }
    printf("ERRORE tipo=%s input=%lld firma=%s file=%s.bin\n", tipo, (long long) g_input_provati, firma,
           g_riproduzione ? "(riproduzione)" : base);
    if (stack) printf("%s", stack);
    fflush(stdout);
    LeaveCriticalSection(&g_cs_rapporto);
}

static const char *nome_eccezione(DWORD c)
{
    switch (c) {
    case EXCEPTION_ACCESS_VIOLATION: return "accesso-memoria-fuori-limiti";
    case EXCEPTION_ILLEGAL_INSTRUCTION: return "istruzione-illegale";
    case EXCEPTION_STACK_OVERFLOW: return "stack-esaurito";
    case EXCEPTION_INT_DIVIDE_BY_ZERO: return "divisione-intera-per-zero";
    case EXCEPTION_INT_OVERFLOW: return "overflow-intero";
    case EXCEPTION_PRIV_INSTRUCTION: return "istruzione-privilegiata";
    case EXCEPTION_ARRAY_BOUNDS_EXCEEDED: return "fuori-limiti";
    case EXCEPTION_DATATYPE_MISALIGNMENT: return "disallineamento";
    case EXCEPTION_IN_PAGE_ERROR: return "errore-pagina";
    case EXCEPTION_GUARD_PAGE: return "pagina-di-guardia";
    case 0xC0000409: return "stack-buffer-overrun";
    default: return NULL;
    }
}

static LONG WINAPI gestore_eccezioni(EXCEPTION_POINTERS *ep)
{
    DWORD c = ep->ExceptionRecord->ExceptionCode;
    const char *tipo = nome_eccezione(c);
    if (!tipo) {
        return EXCEPTION_CONTINUE_SEARCH;
    }
    static char stack[8192], firma[300], dettagli[300];
    stack_testo(GetCurrentThread(), ep->ContextRecord, stack, sizeof(stack), firma, sizeof(firma));
    if (c == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2) {
        ULONG_PTR op = ep->ExceptionRecord->ExceptionInformation[0];
        ULONG_PTR ind = ep->ExceptionRecord->ExceptionInformation[1];
        snprintf(dettagli, sizeof(dettagli), "%s all'indirizzo 0x%llx (codice 0x%08lx)",
                 op == 0 ? "lettura" : (op == 1 ? "scrittura" : "esecuzione"), (unsigned long long) ind, (unsigned long) c);
    } else {
        snprintf(dettagli, sizeof(dettagli), "eccezione 0x%08lx a 0x%llx", (unsigned long) c,
                 (unsigned long long) (uintptr_t) ep->ExceptionRecord->ExceptionAddress);
    }
    rapporto(tipo, firma, dettagli, stack);
    TerminateProcess(GetCurrentProcess(), 3);
    return EXCEPTION_CONTINUE_SEARCH;
}

static void rapporto_qui(const char *tipo, const char *dettagli)
{
    static char stack[8192], firma[300];
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    stack_testo(GetCurrentThread(), &ctx, stack, sizeof(stack), firma, sizeof(firma));
    rapporto(tipo, firma, dettagli, stack);
}

static void su_abort(int sig)
{
    (void) sig;
    rapporto_qui("abort", "abort() chiamata");
    TerminateProcess(GetCurrentProcess(), 3);
}

// -fstack-protector-strong: scrittura oltre un buffer sullo stack.
void __stack_chk_fail(void)
{
    rapporto_qui("scrittura-oltre-buffer-sullo-stack", "stack protector: canarino sovrascritto");
    TerminateProcess(GetCurrentProcess(), 3);
}

// Firme gia' segnalate (UB e controlli del bersaglio): una volta sola.
#define FIRME_MAX 256
static char *g_firme[FIRME_MAX];
static int g_firme_n;

static bool firma_nuova(const char *f)
{
    for (int i = 0; i < g_firme_n; i++) {
        if (strcmp(g_firme[i], f) == 0) return false;
    }
    if (g_firme_n < FIRME_MAX) g_firme[g_firme_n++] = _strdup(f);
    return true;
}

void fuzz_segnala(const char *tipo, const char *dove)
{
    char f[600];
    snprintf(f, sizeof(f), "%s %s", tipo, dove);
    if (!firma_nuova(f)) return;
    static char stack[8192], firma[300];
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    stack_testo(GetCurrentThread(), &ctx, stack, sizeof(stack), firma, sizeof(firma));
    rapporto(tipo, dove, firma, stack);
}

// =============================================================================
// UBSan senza libreria: i gestori li fornisce il motore. Il primo campo dei
// dati passati e' sempre la posizione nel sorgente.
typedef struct { const char *file; uint32_t riga, col; } ubsan_pos_t;

static void ubsan(const char *tipo, const ubsan_pos_t *p)
{
    const char *base = p && p->file ? p->file : "?";
    const char *b1 = strrchr(base, '\\'), *b2 = strrchr(base, '/');
    if (b2 > b1) b1 = b2;
    if (b1) base = b1 + 1;
    char firma[300];
    snprintf(firma, sizeof(firma), "%s:%u:%u", base, p ? p->riga : 0, p ? p->col : 0);
    char chiave[400];
    snprintf(chiave, sizeof(chiave), "UB %s %s", tipo, firma);
    if (!firma_nuova(chiave)) return;
    static char stack[8192], f2[300], dett[600];
    CONTEXT ctx;
    RtlCaptureContext(&ctx);
    stack_testo(GetCurrentThread(), &ctx, stack, sizeof(stack), f2, sizeof(f2));
    snprintf(dett, sizeof(dett), "comportamento indefinito (%s) in %s:%u:%u", tipo, p && p->file ? p->file : "?",
             p ? p->riga : 0, p ? p->col : 0);
    char t[96];
    snprintf(t, sizeof(t), "UB-%s", tipo);
    rapporto(t, firma, dett, stack);
}

#define GESTORE_UB(nome)                                                                        \
    void __ubsan_handle_##nome(void *d, uintptr_t a, uintptr_t b, uintptr_t c)                  \
    { (void) a; (void) b; (void) c; ubsan(#nome, (const ubsan_pos_t *) d); }                    \
    void __ubsan_handle_##nome##_abort(void *d, uintptr_t a, uintptr_t b, uintptr_t c)          \
    { (void) a; (void) b; (void) c; ubsan(#nome, (const ubsan_pos_t *) d); TerminateProcess(GetCurrentProcess(), 3); }

GESTORE_UB(add_overflow)
GESTORE_UB(sub_overflow)
GESTORE_UB(mul_overflow)
GESTORE_UB(negate_overflow)
GESTORE_UB(divrem_overflow)
GESTORE_UB(shift_out_of_bounds)
GESTORE_UB(out_of_bounds)
GESTORE_UB(type_mismatch_v1)
GESTORE_UB(alignment_assumption)
GESTORE_UB(float_cast_overflow)
GESTORE_UB(load_invalid_value)
GESTORE_UB(invalid_builtin)
GESTORE_UB(vla_bound_not_positive)
GESTORE_UB(pointer_overflow)
GESTORE_UB(nonnull_arg)
GESTORE_UB(nonnull_return_v1)
GESTORE_UB(nullability_arg)
GESTORE_UB(nullability_return_v1)
GESTORE_UB(function_type_mismatch)
GESTORE_UB(implicit_conversion)
void __ubsan_handle_builtin_unreachable(void *d) { ubsan("builtin_unreachable", d); TerminateProcess(GetCurrentProcess(), 3); }
void __ubsan_handle_missing_return(void *d) { ubsan("missing_return", d); TerminateProcess(GetCurrentProcess(), 3); }

// =============================================================================
// Sorveglianza del tempo: un input che non finisce = ciclo infinito.
static DWORD WINAPI sorvegliante(void *arg)
{
    (void) arg;
    while (1) {
        Sleep(50);
        if (g_in_esecuzione && GetTickCount64() - g_inizio_input_ms > g_limite_ms) {
            SuspendThread(g_thread_principale);
            CONTEXT ctx;
            memset(&ctx, 0, sizeof(ctx));
            ctx.ContextFlags = CONTEXT_FULL;
            static char stack[8192], firma[300], dett[128];
            if (GetThreadContext(g_thread_principale, &ctx)) {
                stack_testo(g_thread_principale, &ctx, stack, sizeof(stack), firma, sizeof(firma));
            } else {
                strcpy(firma, "sconosciuta");
                stack[0] = '\0';
            }
            snprintf(dett, sizeof(dett), "input in esecuzione da oltre %u ms", g_limite_ms);
            rapporto("ciclo-infinito", firma, dett, stack);
            TerminateProcess(GetCurrentProcess(), 4);
        }
    }
    return 0;
}

// =============================================================================
// Esecuzione di un input
static void esegui(const uint8_t *d, size_t n)
{
    g_corrente = (uint8_t *) d;
    g_corrente_len = n;
    g_zona_usate = 0;
    g_allinea_fine = g_riproduzione ? !g_davanti : !g_allinea_fine;
    uint8_t *buf = fuzz_buffer(n);
    memcpy(buf, d, n);
    g_rng_input = hash_dati(d, n) | 1;
    if (g_mappa_n) memset(g_mappa, 0, g_mappa_n);
    g_inizio_input_ms = GetTickCount64();
    InterlockedExchange(&g_in_esecuzione, 1);
    bersaglio_esegui(buf, n);
    InterlockedExchange(&g_in_esecuzione, 0);
    InterlockedIncrement64(&g_input_provati);
}

// =============================================================================
// Mutazioni
static const int64_t g_valori8[] = { 0, 1, 0x7f, 0x80, 0xff, 0x0a, 0x0d, 0x20, '0', '9', ',', '*', '$', '#', ';', '"' };
static const int64_t g_valori16[] = { 0, 1, 0x7fff, 0x8000, 0xffff, 0x100, 0x3ff, 0x400, 1023, 1024, 4096, 8192, 8193 };
static const int64_t g_valori32[] = { 0, 1, 0x7fffffff, 0x80000000, 0xffffffff, 65535, 65536, -1, 100000, 604800 };
static const char *const g_numeri[] = { "0", "-1", "1", "-0", "99", "4294967296", "2147483648", "-2147483649", "1e308",
                                        "-1e308", "1e-308", "nan", "inf", "-inf", "0.0000000001", "9999999999.99",
                                        "123456789012345678901234567890", "00", ".", "-", "+", "0x10", "1e9", "1e12" };

static size_t inserisci(uint8_t *d, size_t n, size_t max, size_t pos, const uint8_t *src, size_t k)
{
    if (n + k > max) {
        if (n >= max) return n;
        k = max - n;
    }
    memmove(d + pos + k, d + pos, n - pos);
    memcpy(d + pos, src, k);
    return n + k;
}

static size_t sovrascrivi(uint8_t *d, size_t n, size_t max, size_t pos, const uint8_t *src, size_t k)
{
    if (pos + k > n) {
        if (pos + k > max) k = max - pos;
        n = pos + k;
    }
    memcpy(d + pos, src, k);
    return n;
}

static size_t muta_una(uint8_t *d, size_t n, size_t max)
{
    uint8_t tmp[64];
    switch (rnd_n(17)) {
    case 0: // bit
        if (n) d[rnd_n((uint32_t) n)] ^= (uint8_t) (1u << rnd_n(8));
        return n;
    case 1: // byte a caso
        if (n) d[rnd_n((uint32_t) n)] = (uint8_t) rnd();
        return n;
    case 2: // valore limite a 8 bit
        if (n) d[rnd_n((uint32_t) n)] = (uint8_t) g_valori8[rnd_n(sizeof(g_valori8) / sizeof(g_valori8[0]))];
        return n;
    case 3: { // a 16 bit, little o big endian
        if (n < 2) return n;
        uint16_t v = (uint16_t) g_valori16[rnd_n(sizeof(g_valori16) / sizeof(g_valori16[0]))];
        size_t p = rnd_n((uint32_t) (n - 1));
        if (rnd() & 1) { d[p] = (uint8_t) v; d[p + 1] = (uint8_t) (v >> 8); }
        else { d[p] = (uint8_t) (v >> 8); d[p + 1] = (uint8_t) v; }
        return n;
    }
    case 4: { // a 32 bit
        if (n < 4) return n;
        uint32_t v = (uint32_t) g_valori32[rnd_n(sizeof(g_valori32) / sizeof(g_valori32[0]))];
        size_t p = rnd_n((uint32_t) (n - 3));
        for (int i = 0; i < 4; i++) d[p + i] = (uint8_t) ((rnd() & 1) ? v >> (8 * i) : v >> (8 * (3 - i)));
        return n;
    }
    case 5: // piu' o meno qualcosa
        if (n) d[rnd_n((uint32_t) n)] += (uint8_t) (rnd_n(35) - 17);
        return n;
    case 6: { // cancella un pezzo
        if (n < 2) return n;
        size_t k = 1 + rnd_n((uint32_t) ((rnd() & 3) ? (n < 8 ? n - 1 : 8) : n - 1));
        size_t p = rnd_n((uint32_t) (n - k + 1));
        memmove(d + p, d + p + k, n - p - k);
        return n - k;
    }
    case 7: { // inserisci byte a caso o ripetuti
        size_t k = 1 + rnd_n(16);
        uint8_t b = (uint8_t) rnd();
        for (size_t i = 0; i < k; i++) tmp[i] = (rnd() & 1) ? b : (uint8_t) rnd();
        return inserisci(d, n, max, rnd_n((uint32_t) n + 1), tmp, k);
    }
    case 8: { // duplica un pezzo (inserito altrove)
        if (!n) return n;
        size_t k = 1 + rnd_n((uint32_t) (n < 256 ? n : 256));
        size_t p = rnd_n((uint32_t) (n - k + 1));
        uint8_t *c = malloc(k);
        memcpy(c, d + p, k);
        n = inserisci(d, n, max, rnd_n((uint32_t) n + 1), c, k);
        free(c);
        return n;
    }
    case 9: { // copia un pezzo sopra un altro
        if (n < 2) return n;
        size_t k = 1 + rnd_n((uint32_t) (n / 2));
        size_t a = rnd_n((uint32_t) (n - k + 1)), b = rnd_n((uint32_t) (n - k + 1));
        memmove(d + b, d + a, k);
        return n;
    }
    case 10:
    case 11: { // parola del dizionario
        if (!g_diz_n) return n;
        voce_t *v = &g_diz[rnd_n((uint32_t) g_diz_n)];
        size_t p = rnd_n((uint32_t) n + 1);
        return (rnd() & 1) ? inserisci(d, n, max, p, v->d, v->n) : sovrascrivi(d, n, max, p < n ? p : (n ? n - 1 : 0), v->d, v->n);
    }
    case 12: { // incrocio con un altro input del corpus
        if (!g_corpus_n) return n;
        voce_t *v = &g_corpus[rnd_n((uint32_t) g_corpus_n)];
        if (!v->n) return n;
        size_t a = rnd_n((uint32_t) v->n), k = 1 + rnd_n((uint32_t) (v->n - a));
        if (rnd() & 1) {
            return inserisci(d, n, max, rnd_n((uint32_t) n + 1), v->d + a, k);
        }
        size_t taglio = rnd_n((uint32_t) n + 1); // inizio di questo + coda dell'altro
        if (taglio + k > max) k = max - taglio;
        memcpy(d + taglio, v->d + a, k);
        return taglio + k;
    }
    case 13: { // valore confrontato dal firmware
        if (!g_torc_n) return n;
        unsigned i = rnd_n(TORC);
        for (int t = 0; t < 8 && !g_torc[i].size; t++) i = rnd_n(TORC);
        if (!g_torc[i].size) return n;
        uint8_t s = g_torc[i].size > 8 ? 8 : g_torc[i].size;
        for (int j = 0; j < s; j++) tmp[j] = (uint8_t) (g_torc[i].v >> (8 * ((rnd() & 1) ? j : s - 1 - j)));
        if (rnd() & 1) {
            int l = snprintf((char *) tmp, sizeof(tmp), "%llu", (unsigned long long) g_torc[i].v);
            s = (uint8_t) l;
        }
        size_t p = rnd_n((uint32_t) n + 1);
        return (rnd() & 1) ? inserisci(d, n, max, p, tmp, s) : sovrascrivi(d, n, max, p < n ? p : (n ? n - 1 : 0), tmp, s);
    }
    case 14: { // numero in testo
        const char *s = g_numeri[rnd_n(sizeof(g_numeri) / sizeof(g_numeri[0]))];
        size_t p = rnd_n((uint32_t) n + 1);
        return (rnd() & 1) ? inserisci(d, n, max, p, (const uint8_t *) s, strlen(s))
                           : sovrascrivi(d, n, max, p < n ? p : (n ? n - 1 : 0), (const uint8_t *) s, strlen(s));
    }
    case 15: // accorcia
        return n ? rnd_n((uint32_t) n) : 0;
    default: { // cifre in testo cambiate
        for (size_t t = 0; t < 4 && n; t++) {
            size_t p = rnd_n((uint32_t) n);
            if (d[p] >= '0' && d[p] <= '9') d[p] = (uint8_t) ('0' + rnd_n(10));
        }
        return n;
    }
    }
}

static size_t muta(uint8_t *d, size_t n, size_t max)
{
    int k = 1 << rnd_n(4);
    for (int i = 0; i < k; i++) {
        n = muta_una(d, n, max);
    }
    return n;
}

// =============================================================================
static uint8_t *leggi_file(const char *p, size_t *n)
{
    FILE *f = fopen(p, "rb");
    if (!f) return NULL;
    fseek(f, 0, SEEK_END);
    long l = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *d = malloc(l > 0 ? (size_t) l : 1);
    *n = fread(d, 1, (size_t) (l > 0 ? l : 0), f);
    fclose(f);
    return d;
}

int main(int argc, char **argv)
{
    double secondi = 60;
    long long max_input = -1;
    const char *riproduci = NULL;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-nome") && i + 1 < argc) g_nome = argv[++i];
        else if (!strcmp(argv[i], "-tempo") && i + 1 < argc) secondi = atof(argv[++i]);
        else if (!strcmp(argv[i], "-seme") && i + 1 < argc) g_seme = strtoull(argv[++i], NULL, 10);
        else if (!strcmp(argv[i], "-crash") && i + 1 < argc) g_dir_crash = argv[++i];
        else if (!strcmp(argv[i], "-input") && i + 1 < argc) max_input = atoll(argv[++i]);
        else if (!strcmp(argv[i], "-riproduci") && i + 1 < argc) riproduci = argv[++i];
        else if (!strcmp(argv[i], "-diagnosi")) g_diagnosi = true;
        else {
            fprintf(stderr, "uso: %s [-nome N] [-tempo s] [-seme n] [-crash cartella] [-input quanti] [-riproduci file]\n", argv[0]);
            return 2;
        }
    }
    SYSTEM_INFO si;
    GetSystemInfo(&si);
    g_pagina = si.dwPageSize;
    InitializeCriticalSection(&g_cs_rapporto);

    g_sym_ok = carica_dbghelp();
    ULONG garanzia = 64 * 1024;
    SetThreadStackGuarantee(&garanzia); // spazio per il rapporto anche a stack esaurito
    AddVectoredExceptionHandler(1, gestore_eccezioni);
    signal(SIGABRT, su_abort);
    DuplicateHandle(GetCurrentProcess(), GetCurrentThread(), GetCurrentProcess(), &g_thread_principale, 0, FALSE,
                    DUPLICATE_SAME_ACCESS);
    zone_crea();
    g_rng ^= g_seme * 0x9E3779B97F4A7C15ull;
    if (!g_rng) g_rng = 1;
    setvbuf(stdout, NULL, _IOLBF, 4096);

    bersaglio_avvio();
    CreateThread(NULL, 0, sorvegliante, NULL, 0, NULL);

    if (riproduci) {
        g_riproduzione = true;
        size_t n = 0;
        uint8_t *d = leggi_file(riproduci, &n);
        if (!d) {
            fprintf(stderr, "non riesco a leggere %s\n", riproduci);
            return 2;
        }
        g_limite_ms *= 5;
        // Due giri: buffer contro la pagina protetta in fondo, poi all'inizio.
        esegui(d, n);
        if (!g_errori_trovati) {
            g_davanti = true;
            esegui(d, n);
        }
        printf(g_errori_trovati ? "RIPRODOTTO (%d errori)\n" : "NESSUN ERRORE\n", g_errori_trovati);
        return g_errori_trovati ? 3 : 0;
    }

    bersaglio_semi();
    if (!g_semi_n) fuzz_seme("", 0);
    uint8_t *lavoro = malloc(g_max_len + 1);
    for (size_t i = 0; i < g_semi_n; i++) {
        size_t n = g_semi[i].n < g_max_len ? g_semi[i].n : g_max_len;
        memcpy(lavoro, g_semi[i].d, n);
        esegui(lavoro, n);
        if (g_diagnosi) {
            unsigned nz = 0;
            for (size_t q = 0; q < g_mappa_n; q++) nz += g_mappa[q] != 0;
            printf("seme %zu: %zu byte, %u blocchi percorsi su %zu strumentati\n", i, n, nz, g_mappa_n);
        }
        copertura_nuova();
        corpus_aggiungi(lavoro, n);
    }

    ULONGLONG t0 = GetTickCount64(), ultimo = t0;
    while (1) {
        ULONGLONG ora = GetTickCount64();
        if ((secondi > 0 && (ora - t0) >= secondi * 1000.0) || (max_input >= 0 && g_input_provati >= max_input)) break;
        if (ora - ultimo >= 10000) {
            ultimo = ora;
            printf("  ... %s: %lld input, corpus %zu, copertura %u, errori %d, %.0f s\n", g_nome,
                   (long long) g_input_provati, g_corpus_n, caselle_viste(), g_errori_trovati, (ora - t0) / 1000.0);
        }
        // Scelta: meta' delle volte tra gli ultimi aggiunti (strade appena scoperte).
        size_t i = (rnd() & 1) || g_corpus_n < 32 ? rnd_n((uint32_t) g_corpus_n)
                                                   : g_corpus_n - 1 - rnd_n(32);
        size_t n = g_corpus[i].n;
        memcpy(lavoro, g_corpus[i].d, n);
        n = muta(lavoro, n, g_max_len);
        esegui(lavoro, n);
        if (copertura_nuova()) {
            corpus_aggiungi(lavoro, n);
        }
    }
    printf("RISULTATO nome=%s input=%lld corpus=%zu copertura=%u errori=%d tempo=%.0f\n", g_nome,
           (long long) g_input_provati, g_corpus_n, caselle_viste(), g_errori_trovati, (GetTickCount64() - t0) / 1000.0);
    return 0;
}

#pragma clang attribute pop
