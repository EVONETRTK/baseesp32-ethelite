// Fuzzing di main/nmea_etrf.c: riscrittura delle righe GGA/RMC in ETRF2000
// (anche con buffer di uscita appena sufficienti) e lettura dei campi GGA.
// Input: [opzioni][margine][epoca][qualita'][lat 8][lon 8][quota 8][eta' 4][riga]
//   opzioni bit 0: checksum ricalcolato; bit 1-2: 0 convert, 1 set_position,
//   2 entrambe; bit 3: lat/lon/quota dai byte (anche NaN/inf) invece che
//   valori plausibili.
// Il buffer della riga e' lungo strlen + 1 + margine (0..31) e finisce
// contro una pagina protetta: una scrittura oltre cap = crash.
// SORGENTI: etrf.c
#include "motore.h"
#include "aiuti.h"
#include "../../../main/nmea_etrf.c"

#include <math.h>

#define TESTA 32

void bersaglio_avvio(void)
{
    fuzz_imposta_max_len(600);
}

static void seme(uint8_t opz, uint8_t margine, double lat, double lon, double h, const char *riga)
{
    uint8_t b[700] = { opz, margine, 65, 2 };
    memcpy(b + 4, &lat, 8);
    memcpy(b + 12, &lon, 8);
    memcpy(b + 20, &h, 8);
    float eta = 5.0f;
    memcpy(b + 28, &eta, 4);
    size_t n = strlen(riga);
    memcpy(b + TESTA, riga, n);
    fuzz_seme(b, TESTA + n);
}

void bersaglio_semi(void)
{
    static const char *const righe[] = {
        "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,*43",
        "$GNRMC,101010.00,A,4100.0000000,N,01630.0000000,E,0.01,0.0,091026,,,A,V*07",
        "$GPGGA,000001.00,3352.1234567,S,07038.7654321,W,1,08,1.2,12.3,M,-25.1,M,,*79",
        "$GNGGA,123519.00,4807.038,N,01131.000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57",
        "$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5D",
        "$GNRMC,235959.99,A,8959.9999999,N,17959.9999999,W,0.01,0.0,311226,,,A,V*00",
    };
    for (size_t i = 0; i < sizeof(righe) / sizeof(righe[0]); i++) {
        for (uint8_t m = 0; m < 3; m++) {
            seme((uint8_t) (1 | (m << 1)), (uint8_t) (m * 7), 41.0 + i * 0.1, 16.5 - i, 450.0, righe[i]);
        }
        seme(0, 0, 0, 0, 0, righe[i]);
    }
    static const char *const parole[] = { "$GNGGA,", "$GNRMC,", ",N,", ",S,", ",E,", ",W,", ",M,", ",A,", ".0000000",
                                          "*", ",,", "99999.9999999", "-0.5" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

static bool checksum_valido(const char *r)
{
    if (r[0] != '$') return false;
    const char *star = strchr(r, '*');
    if (!star || strlen(star) != 3) return false;
    unsigned cs = 0;
    for (const char *p = r + 1; p < star; p++) cs ^= (unsigned char) *p;
    char atteso[4];
    snprintf(atteso, sizeof(atteso), "%02X", cs);
    return strcmp(star + 1, atteso) == 0;
}

static void prova_riscrittura(const char *riga, size_t cap, int quale, const uint8_t *t, bool grezzi)
{
    char *line = (char *) fuzz_buffer(cap);
    memcpy(line, riga, strlen(riga) + 1);
    double epoca = 2020.0 + t[2] / 20.0;
    double lat, lon, h;
    float eta;
    if (grezzi) {
        memcpy(&lat, t + 4, 8);
        memcpy(&lon, t + 12, 8);
        memcpy(&h, t + 20, 8);
    } else {
        // valori plausibili ricavati dai byte
        lat = ((int8_t) t[4]) * 0.7 + t[5] / 300.0;
        lon = ((int8_t) t[12]) * 1.4 + t[13] / 300.0;
        h = ((int16_t) (t[20] | (t[21] << 8))) / 10.0;
    }
    memcpy(&eta, t + 28, 4);
    int q = (int8_t) t[3];
    bool ok = quale == 0 ? nmea_etrf_convert(line, cap, epoca, h)
                         : nmea_etrf_set_position(line, cap, epoca, lat, lon, h, q, eta);
    if (ok) {
        FUZZ_VERIFICA(strlen(line) < cap);
        FUZZ_VERIFICA(checksum_valido(line));
    } else {
        FUZZ_VERIFICA(strcmp(line, riga) == 0); // riga invariata se non riscritta
    }
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (n < TESTA) return;
    const uint8_t *t = d;
    uint8_t opz = d[0];
    char *riga = (char *) aiuto_copia(d + TESTA, n - TESTA);
    size_t len = strlen(riga);
    if (opz & 1) {
        aiuto_nmea_checksum(riga, len + 4);
        len = strlen(riga);
    }
    // lettura dei campi, su una copia contro la pagina protetta
    char *s = fuzz_stringa(riga, len);
    double a, b;
    (void) nmea_gga_quality(s);
    (void) nmea_gga_ellipsoidal_height(s, &a);
    (void) nmea_gga_latlon(s, &a, &b);

    size_t cap = len + 1 + (t[1] % 32);
    int quale = (opz >> 1) & 3;
    bool grezzi = (opz >> 3) & 1;
    if (quale == 0 || quale >= 2) prova_riscrittura(riga, cap, 0, t, grezzi);
    if (quale >= 1) prova_riscrittura(riga, cap, 1, t, grezzi);
}
