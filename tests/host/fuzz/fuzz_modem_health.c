// Fuzzing di main/modem_health.c: lettura delle risposte del SIM7600 a
// AT+CPMUTEMP, AT+CBC, AT+CPSI? e AT+CGMR.
// Input: [opzioni][risposta]. Opzioni: misura del buffer per la versione
// del firmware (1..64).
#include "motore.h"

#include "../../../main/modem_health.c"

void bersaglio_avvio(void)
{
    fuzz_imposta_max_len(600);
}

static void seme(const char *r)
{
    uint8_t b[600];
    size_t l = strlen(r);
    b[0] = 40;
    memcpy(b + 1, r, l);
    fuzz_seme(b, l + 1);
}

void bersaglio_semi(void)
{
    seme("AT+CPMUTEMP\r\r\n+CPMUTEMP: 36\r\n\r\nOK\r\n");
    seme("AT+CBC\r\r\n+CBC: 3.801V\r\n\r\nOK\r\n");
    seme("+CBC: 0,85,3912\r\n");
    seme("AT+CPSI?\r\r\n+CPSI: LTE,Online,222-88,0x5A1E,187214780,257,EUTRAN-BAND20,6300,5,5,-94,-850,-545,15\r\n\r\nOK\r\n");
    seme("+CPSI: WCDMA,Online,222-01,0x0000,12345,WCDMA IMT 2000,10688,0,0,-90,-5,0,0,0\r\n");
    seme("+CPSI: NO SERVICE,Online\r\n");
    seme("AT+CGMR\r\r\n+CGMR: LE20B04SIM7600M22\r\n\r\nOK\r\n");
    static const char *const parole[] = { "+CPMUTEMP: ", "+CBC: ", "+CPSI: ", "+CGMR: ", "LTE", "Online",
                                          "EUTRAN-BAND", "V", ",", "-", ".", "\r\n", "OK", "ERROR", "0x" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    size_t cap = 1 + d[0] % 64;
    const char *r = (const char *) d + 1;
    size_t l = n - 1;

    int t, mv;
    (void) modem_health_parse_temp(fuzz_stringa(r, l), &t);
    (void) modem_health_parse_cbc(fuzz_stringa(r, l), &mv);

    modem_health_t *h = (modem_health_t *) fuzz_buffer(sizeof(modem_health_t));
    memset(h, 0, sizeof(*h));
    if (modem_health_parse_cpsi(fuzz_stringa(r, l), h)) {
        FUZZ_VERIFICA(memchr(h->mode, 0, sizeof(h->mode)) != NULL);
        FUZZ_VERIFICA(memchr(h->mcc_mnc, 0, sizeof(h->mcc_mnc)) != NULL);
    }

    char *fw = (char *) fuzz_buffer(cap);
    if (modem_health_parse_cgmr(fuzz_stringa(r, l), fw, cap)) FUZZ_VERIFICA(strlen(fw) < cap);
}
