// Fuzzing di main/base_stream_demux.c (separazione del flusso della base in
// RTCM3 / righe NMEA e ASCII / UBX), con i lettori veri a valle: gnss_fix
// (GGA), gnss_signal (GSV), gnss_unicore e gnss_comnav (righe '#').
// Input: [opzioni][flusso]. Opzione bit 0: controlli (CRC24Q, Fletcher,
// checksum NMEA, CRC32 ASCII) ricalcolati dopo la mutazione. Il flusso
// arriva a pezzi di misura casuale (1..64 byte o tutto insieme), ognuno in
// un buffer contro una pagina protetta.
// SORGENTI: gnss_fix.c gnss_signal.c gnss_unicore.c gnss_comnav.c
#include "motore.h"
#include "aiuti.h"
#include "stub_gnss.h"
#include "../../../main/base_stream_demux.c"

void bersaglio_avvio(void)
{
    gnss_fix_init();
    gnss_signal_init();
    fuzz_imposta_max_len(4096);
}

static const uint8_t F1005[] = {
    0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x03, 0x8A, 0xC5, 0x4F, 0x99, 0xD2, 0x03,
    0x30, 0x95, 0x7F, 0x2E, 0x09, 0xB0, 0xBE, 0xF0, 0x34, 0xF6, 0x3D, 0x42,
};
static const uint8_t F1077[] = {
    0xD3, 0x00, 0x09, 0x43, 0x51, 0x23, 0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62, 0x0F, 0xA7, 0xF9,
};
static const uint8_t ACK[] = { 0xB5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x8A, 0x98, 0xC1 };
static const char *GGA = "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57\r\n";
static const char *GSV = "$GNGSV,1,1,01,05,45,120,40,1*48\r\n";
static const char *GSV2 = "$GPGSV,3,1,11,02,45,120,40,05,10,300,22,12,80,010,48,25,33,215,35,1*6A\r\n";
static const char *PPP =
    "#PPPNAVA,COM1,0,80.0,FINE,2400,123456.000,0,0,18,0;SOL_COMPUTED,PPP_CONVERGING,41.12345678901,"
    "16.98765432101,409.5000,40.5000,WGS84,0.1234,0.2345,0.3456,\"0\",5.000,0.000,30,25,0,0,0,0,0,0*8fa5f3fa\r\n";
static const char *VER =
    "#VERSIONA,COM1,0,80.0,FINE,2400,123459.000,0,0,18,0;\"UM982\",\"R4.10Build99999\",\"COM1\",\"ffff\",\"2026/01/01\"*23abc4dc\r\n";
static const char *VERK =
    "#VERSIONA,COM1,0,55.0,FINESTEERING,2380,1000.000,00000000,0000,0;1,GPSCARD,\"K922\",\"00000000\",\"CARD-1\",\"1.10A\"*00000000\r\n";

static uint8_t s_seme[4096];
static size_t s_n;
static void metti(const void *p, size_t n) { memcpy(s_seme + s_n, p, n); s_n += n; }
static void metti_str(const char *s) { metti(s, strlen(s)); }

void bersaglio_semi(void)
{
    for (int opz = 0; opz < 2; opz++) {
        s_n = 0;
        uint8_t o = (uint8_t) opz;
        metti(&o, 1);
        metti_str(GGA);
        metti(F1005, sizeof(F1005));
        metti(ACK, sizeof(ACK));
        metti_str(GSV);
        metti(F1077, sizeof(F1077));
        metti_str(PPP);
        metti_str(GSV2);
        metti(F1005, sizeof(F1005));
        metti_str(VER);
        metti_str(VERK);
        fuzz_seme(s_seme, s_n);
    }
    const uint8_t spuri[] = { 1, 0x00, 0xFF, 0x0A, 0x0D, 0xB5, 0x41, 0x7E, 0x80, 0xB5, 0xD3, 0xD3, 0x00 };
    fuzz_seme(spuri, sizeof(spuri));
    fuzz_parola_bin(F1005, 3);
    fuzz_parola_bin(ACK, 6);
    fuzz_parola("$GNGGA,");
    fuzz_parola("$GPGSV,");
    fuzz_parola("#PPPNAVA,");
    fuzz_parola("#BESTPOSA,");
    fuzz_parola("#VERSIONA,");
    fuzz_parola("\r\n");
    fuzz_parola(";SOL_COMPUTED,PPP,");
}

static void su_rtcm(const uint8_t *f, size_t len)
{
    // Un frame passato al caster deve essere completo e con il CRC giusto.
    FUZZ_VERIFICA(len >= 6 && f[0] == 0xD3);
    if (len < 6) return;
    size_t plen = ((size_t) (f[1] & 3) << 8) | f[2];
    FUZZ_VERIFICA(len == plen + 6);
    uint32_t c = aiuto_crc24q(f, len - 3);
    FUZZ_VERIFICA(f[len - 3] == (uint8_t) (c >> 16) && f[len - 2] == (uint8_t) (c >> 8) && f[len - 1] == (uint8_t) c);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    uint8_t *f = aiuto_copia(d + 1, n - 1);
    n--;
    if (opz & 1) aiuto_aggiusta_flusso(f, n);
    // stato da capo: ogni input e' un flusso nuovo (ripetibile)
    s_state = ST_IDLE;
    s_replaying = false;
    size_t i = 0;
    bool intero = (opz & 6) == 0;
    while (i < n) {
        size_t k = intero ? n : 1 + fuzz_caso() % 64;
        if (k > n - i) k = n - i;
        uint8_t *b = fuzz_buffer(k);
        memcpy(b, f + i, k);
        base_stream_demux_feed(b, k, su_rtcm);
        i += k;
        if (!intero) fuzz_zone_azzera(); // zone limitate: si riusano
    }
}
