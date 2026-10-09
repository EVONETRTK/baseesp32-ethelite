// Fuzzing di main/gnss_signal.c (righe NMEA GSV: satelliti e segnale per
// il grafico del pannello).
// Input: [opzioni][righe separate da '\n']. Ogni riga in un buffer contro
// una pagina protetta; tra una riga e l'altra il tempo avanza di 0..0,5 s
// (bit 0 delle opzioni: tutte nella stessa epoca). Alla fine si leggono i
// satelliti in un buffer di misura a caso.
#include "motore.h"
#include "aiuti.h"
#include "../../../main/gnss_signal.c"

void host_time_advance_us(int64_t us);

void bersaglio_avvio(void)
{
    gnss_signal_init();
    fuzz_imposta_max_len(3000);
}

void bersaglio_semi(void)
{
    static const char *const flussi[] = {
        "\x00$GPGSV,3,1,11,02,45,120,40,05,10,300,22,12,80,010,48,25,33,215,35,1*6A\n"
        "$GPGSV,3,2,11,29,05,045,,31,60,090,44,32,20,180,30,10,15,270,28,1*60\n"
        "$GPGSV,3,3,11,13,70,330,45,15,25,060,33,18,40,150,39,1*55\n"
        "$GLGSV,1,1,03,65,30,100,35,66,50,200,41,72,10,350,20,1*7F\n"
        "$GAGSV,1,1,02,04,55,100,44,09,35,250,38,7*70\n"
        "$GPGSV,1,1,00,0*65\n",
        "\x01$GNGSV,1,1,01,05,45,120,40,1*48\n$GPGSV,1,1,04,02,45,120,40,05,10,300,22,12,80,010,48,25,33,215,*6A\n",
    };
    fuzz_seme(flussi[0], strlen(flussi[0] + 1) + 1);
    fuzz_seme(flussi[1], strlen(flussi[1] + 1) + 1);
    fuzz_parola("$GPGSV,");
    fuzz_parola("$GLGSV,");
    fuzz_parola(",1,");
    fuzz_parola(",,");
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    d++;
    n--;
    // stato da capo
    memset(s_sats, 0, sizeof(s_sats));
    memset(s_clears, 0, sizeof(s_clears));
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && d[j] != '\n') j++;
        char *s = fuzz_stringa(d + i, j - i);
        gnss_signal_parse_gsv(s);
        if (!(opz & 1)) host_time_advance_us((int64_t) (fuzz_caso() % 500000));
        i = j + 1;
        fuzz_zone_azzera();
    }
    size_t max = (opz >> 1) % 80;
    gnss_sat_signal_t *out = (gnss_sat_signal_t *) fuzz_buffer(max * sizeof(gnss_sat_signal_t));
    size_t got = gnss_signal_get_satellites(out, max);
    FUZZ_VERIFICA(got <= max && got <= GNSS_SIGNAL_MAX_SATS);
}
