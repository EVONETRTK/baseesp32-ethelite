// Fuzzing di main/gnss_fix.c: nmea_checksum_ok e gnss_fix_parse_gga.
// Input: [opzioni][riga]. Opzione bit 0: checksum ricalcolato dopo la
// mutazione (la riga arriva alla lettura dei campi).
#include "motore.h"
#include "aiuti.h"
#include "../../../main/gnss_fix.c"

void bersaglio_avvio(void)
{
    gnss_fix_init();
    fuzz_imposta_max_len(400);
}

static void seme(uint8_t opz, const char *riga)
{
    uint8_t b[512];
    b[0] = opz;
    size_t n = strlen(riga);
    memcpy(b + 1, riga, n);
    fuzz_seme(b, n + 1);
}

void bersaglio_semi(void)
{
    static const char *const righe[] = {
        "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57",
        "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,5,12,0.8,545.4,M,46.9,M,2.0,0000*55",
        "$GPGGA,000001.00,3352.1234567,S,07038.7654321,W,1,08,1.2,12.3,M,-25.1,M,,*79",
        "$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5D",
        "$GNGSV,1,1,01,05,45,120,40,1*48",
    };
    for (size_t i = 0; i < sizeof(righe) / sizeof(righe[0]); i++) {
        seme(0, righe[i]);
        seme(1, righe[i]);
    }
    static const char *const parole[] = { "$GNGGA", "$GPGGA", ",", "*", ",N,", ",S,", ",E,", ",W,", ",M,", ".", "-" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    d++;
    n--;
    char *s;
    if (opz & 1) {
        char *c = (char *) aiuto_copia(d, n);
        aiuto_nmea_checksum(c, n + 4 < 65536 ? n + 4 : 65536);
        s = fuzz_stringa(c, strlen(c));
    } else {
        s = fuzz_stringa(d, n);
    }
    (void) nmea_checksum_ok(s);
    gnss_fix_parse_gga(s);
    gnss_fix_status_t st = gnss_fix_get_status();
    (void) gnss_fix_quality_str(st.quality);
}
