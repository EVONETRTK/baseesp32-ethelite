// Fuzzing di main/gnss_unicore.c (righe ASCII #PPPNAVA / #BESTPOSA /
// #VERSIONA: CRC32 e lettura dei campi) e di gnss_comnav_note_line
// (#VERSIONA dei ComNav).
// Input: [opzioni][riga]. Opzione bit 0: CRC32 ricalcolato dopo la
// mutazione. Nota: dopo 20 righe con il CRC sbagliato di fila il firmware
// smette di controllarlo fino al riavvio: succede anche qui, durante il giro.
// SORGENTI: gnss_comnav.c
#include "motore.h"
#include "aiuti.h"
#include "stub_gnss.h"
#include "../../../main/gnss_unicore.c"
#include "gnss_comnav.h"

#include <math.h>

void bersaglio_avvio(void)
{
    fuzz_imposta_max_len(700);
}

static void seme(uint8_t opz, const char *riga)
{
    uint8_t b[800];
    b[0] = opz;
    size_t n = strlen(riga);
    memcpy(b + 1, riga, n);
    fuzz_seme(b, n + 1);
}

void bersaglio_semi(void)
{
    static const char *const righe[] = {
        "#PPPNAVA,COM1,0,80.0,FINE,2400,123456.000,0,0,18,0;SOL_COMPUTED,PPP_CONVERGING,41.12345678901,"
        "16.98765432101,409.5000,40.5000,WGS84,0.1234,0.2345,0.3456,\"0\",5.000,0.000,30,25,0,0,0,0,0,0*8fa5f3fa",
        "#PPPNAVA,COM1,0,80.0,FINE,2400,123457.000,0,0,18,0;INSUFFICIENT_OBS,NONE,0.00000000000,0.00000000000,"
        "0.0000,0.0000,WGS84,0.0000,0.0000,0.0000,\"0\",0.000,0.000,5,0,0,0,0,0,0,0*b50bc17b",
        "#BESTPOSA,COM1,0,80.0,FINE,2400,123458.000,0,0,18,0;SOL_COMPUTED,PPP,-33.50000000000,-70.25000000000,"
        "500.0000,25.0000,WGS84,0.0500,0.0600,0.0900,\"0\",3.000,0.000,32,28,0,0,0,0,0,0*e558ee8e",
        "#VERSIONA,COM1,0,80.0,FINE,2400,123459.000,0,0,18,0;\"UM982\",\"R4.10Build99999\",\"COM1\",\"ffff\",\"2026/01/01\"*23abc4dc",
        "#VERSIONA,COM1,0,55.0,FINESTEERING,2380,1000.000,00000000,0000,0;1,GPSCARD,\"K922\",\"00000000\",\"CARD-1\",\"1.10A\"*00000000",
    };
    for (size_t i = 0; i < sizeof(righe) / sizeof(righe[0]); i++) {
        seme(0, righe[i]);
        seme(1, righe[i]);
        seme(2, righe[i]);
    }
    static const char *const parole[] = { "#PPPNAVA,", "#BESTPOSA,", "#VERSIONA,", ";", ",", "\"", "*", "SOL_COMPUTED",
                                          "PPP_CONVERGING", "PPP", "\"UM982\"", "\"K9", "COMNAV" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    char *c = (char *) aiuto_copia(d + 1, n - 1);
    size_t len = strlen(c);
    if (opz & 1) {
        aiuto_unicore_crc(c, len + 12);
        len = strlen(c);
    }
    char *s = fuzz_stringa(c, len);
    gnss_unicore_note_line(s);
    gnss_comnav_note_line(s);
    gnss_unicore_ppp_t p;
    gnss_unicore_ppp_get(&p);
    FUZZ_VERIFICA(strlen(p.type) < sizeof(p.type));
    FUZZ_VERIFICA(strlen(gnss_unicore_model()) < sizeof(s_model));
    FUZZ_VERIFICA(strlen(gnss_unicore_fw()) < sizeof(s_fw));
}
