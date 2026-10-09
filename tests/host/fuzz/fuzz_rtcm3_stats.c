// Fuzzing di main/rtcm3_stats.c (conteggio dei byte per tipo di messaggio
// RTCM3, flusso grezzo senza controllo del CRC).
// Input: [opzioni][flusso]. Flusso a pezzi di misura casuale, ognuno contro
// una pagina protetta; poi lettura della tabella con un massimo a caso.
#include "motore.h"
#include "aiuti.h"
#include "../../../main/rtcm3_stats.c"

void bersaglio_avvio(void)
{
    rtcm3_stats_init();
    fuzz_imposta_max_len(4096);
}

void bersaglio_semi(void)
{
    static const uint8_t F1005[] = {
        0, 0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x03, 0x8A, 0xC5, 0x4F, 0x99, 0xD2, 0x03,
        0x30, 0x95, 0x7F, 0x2E, 0x09, 0xB0, 0xBE, 0xF0, 0x34, 0xF6, 0x3D, 0x42,
        0xD3, 0x00, 0x09, 0x43, 0x51, 0x23, 0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62, 0x0F, 0xA7, 0xF9,
    };
    fuzz_seme(F1005, sizeof(F1005));
    uint8_t b[600];
    size_t n = 0;
    b[n++] = 2;
    for (int t = 0; t < 30; t++) { // piu' tipi di quanti ne stanno nella tabella
        uint16_t tipo = (uint16_t) (1001 + t * 7);
        uint8_t f[9] = { 0xD3, 0x00, 0x03, (uint8_t) (tipo >> 4), (uint8_t) ((tipo & 0x0F) << 4), 0x00, 0, 0, 0 };
        memcpy(b + n, f, sizeof(f));
        n += sizeof(f);
    }
    fuzz_seme(b, n);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    d++;
    n--;
    rtcm3_stats_init();
    size_t i = 0;
    bool intero = (opz & 6) == 0;
    while (i < n) {
        size_t k = intero ? n : 1 + fuzz_caso() % 48;
        if (k > n - i) k = n - i;
        uint8_t *b = fuzz_buffer(k);
        memcpy(b, d + i, k);
        rtcm3_stats_feed(b, k);
        i += k;
        fuzz_zone_azzera();
    }
    size_t max = opz >> 3; // 0..31
    rtcm3_stat_entry_t *e = (rtcm3_stat_entry_t *) fuzz_buffer(max * sizeof(rtcm3_stat_entry_t));
    size_t got = rtcm3_stats_get(e, max);
    FUZZ_VERIFICA(got <= max);
}
