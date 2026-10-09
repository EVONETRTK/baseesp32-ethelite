// Fuzzing di main/rtcm3_1005.c (lettura della posizione della base dai
// frame RTCM3 1005/1006 nel flusso grezzo).
// Input: [opzioni][flusso]. Opzione bit 0: CRC24Q ricalcolato dopo la
// mutazione (il frame arriva alla decodifica). Flusso a pezzi di misura
// casuale, ognuno contro una pagina protetta.
#include "motore.h"
#include "aiuti.h"
#include "../../../main/rtcm3_1005.c"

#include <math.h>

void bersaglio_avvio(void)
{
    rtcm3_1005_init();
    fuzz_imposta_max_len(3000);
}

static const uint8_t F1005[] = {
    0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x03, 0x8A, 0xC5, 0x4F, 0x99, 0xD2, 0x03,
    0x30, 0x95, 0x7F, 0x2E, 0x09, 0xB0, 0xBE, 0xF0, 0x34, 0xF6, 0x3D, 0x42,
};
static const uint8_t F1006[] = {
    0xD3, 0x00, 0x15, 0x3E, 0xEF, 0xFF, 0x03, 0xB9, 0xB6, 0xAC, 0xB1, 0xFF, 0x35, 0xFC,
    0xFF, 0xEA, 0xF1, 0x08, 0xF6, 0xC7, 0x74, 0x88, 0x3A, 0x98, 0x79, 0x17, 0xCC,
};
static const uint8_t F1077[] = {
    0xD3, 0x00, 0x09, 0x43, 0x51, 0x23, 0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62, 0x0F, 0xA7, 0xF9,
};

void bersaglio_semi(void)
{
    uint8_t b[256];
    for (int opz = 0; opz < 2; opz++) {
        size_t n = 0;
        b[n++] = (uint8_t) opz;
        memcpy(b + n, F1005, sizeof(F1005)); n += sizeof(F1005);
        memcpy(b + n, F1077, sizeof(F1077)); n += sizeof(F1077);
        memcpy(b + n, F1006, sizeof(F1006)); n += sizeof(F1006);
        fuzz_seme(b, n);
    }
    fuzz_parola_bin(F1005, 5);
    fuzz_parola_bin(F1006, 5);
    fuzz_parola_bin(F1077, 5);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    uint8_t *f = aiuto_copia(d + 1, n - 1);
    n--;
    if (opz & 1) aiuto_aggiusta_flusso(f, n);
    rtcm3_1005_init();
    size_t i = 0;
    bool intero = (opz & 6) == 0;
    while (i < n) {
        size_t k = intero ? n : 1 + fuzz_caso() % 48;
        if (k > n - i) k = n - i;
        uint8_t *b = fuzz_buffer(k);
        memcpy(b, f + i, k);
        rtcm3_position_t p;
        if (rtcm3_1005_feed(b, k, &p)) {
            // 38 bit con segno * 0,0001 m: sempre entro +-13,7 milioni di km... di 0,1 mm
            FUZZ_VERIFICA(fabs(p.ecef_x_m) < 1.4e7 && fabs(p.ecef_y_m) < 1.4e7 && fabs(p.ecef_z_m) < 1.4e7);
        }
        i += k;
        fuzz_zone_azzera();
    }
}
