// Fuzzing di main/gnss_nmea_reader.c (task che legge il ricevitore in modo
// rover: righe NMEA e ASCII, frame UBX interposti, riserva Galileo HAS con
// riscrittura ETRF2000), con i moduli veri a valle.
// Il task e' un ciclo infinito: gnss_io_read() gli passa l'input a pezzi di
// misura casuale e, a input finito, torna qui con longjmp.
// In piu' le funzioni interne (nmea_rmc_time, bynav_data_field,
// has_fallback_line) sono chiamate direttamente con la riga contro una
// pagina protetta, per scoprire letture oltre la fine della riga.
// Input: [opzioni][flusso]. Opzioni: bit 0 controlli ricalcolati (checksum
// NMEA, CRC32 ASCII, UBX), bit 1 riserva HAS attiva, bit 2 RTCM recente
// (RTK valido), bit 3 finestra UBX aperta, bit 4 ora non ancora sincronizzata.
// SORGENTI: gnss_fix.c gnss_signal.c gnss_ubx_ack.c gnss_unicore.c gnss_comnav.c nmea_etrf.c etrf.c
#include "motore.h"
#include "aiuti.h"
#include "stub_gnss.h"
#include "../../../main/gnss_nmea_reader.c"

#include <setjmp.h>

// --- sostituti ------------------------------------------------------------------
static jmp_buf g_fine;
static const uint8_t *g_flusso;
static size_t g_flusso_n, g_flusso_i;
static uint8_t g_opz;

int gnss_io_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    (void) timeout;
    if (g_flusso_i >= g_flusso_n) longjmp(g_fine, 1);
    size_t k = 1 + fuzz_caso() % 300;
    if (k > len) k = len;
    if (k > g_flusso_n - g_flusso_i) k = g_flusso_n - g_flusso_i;
    memcpy(buf, g_flusso + g_flusso_i, k);
    g_flusso_i += k;
    return (int) k;
}

void sys_stats_heartbeat(heartbeat_t which) { (void) which; }
bool time_sync_wants_gnss(void) { return (g_opz & 16) != 0; }
static volatile int g_ora_letta; // i valori si usano: il compilatore non salta le letture
void time_sync_from_gnss(int year, int month, int day, int hour, int min, int sec)
{
    g_ora_letta = year + month + day + hour + min + sec;
}
void nmea_udp_broadcast_send(const char *line, size_t len)
{
    FUZZ_VERIFICA(len == strlen(line)); // lunghezza coerente con la riga (anche dopo la riscrittura)
}
int64_t status_get_last_rtcm_time_us(void) { return (g_opz & 4) ? esp_timer_get_time() - 1000000 : 0; }
void ntrip_rover_client_forward_gga(const char *line, size_t len) { FUZZ_VERIFICA(len == strlen(line)); }
void status_bynav_ins_note_inspvaxa(const char *ins_status, float roll_deg, float pitch_deg, float heading_deg)
{
    FUZZ_VERIFICA(strlen(ins_status) < 24);
    (void) roll_deg; (void) pitch_deg; (void) heading_deg;
}
void status_bynav_ins_note_headinga(float heading_deg, float pitch_deg) { (void) heading_deg; (void) pitch_deg; }
void status_svin_note(bool active, bool valid, uint32_t duration_s, float mean_acc_m, uint32_t observations)
{
    (void) active; (void) valid; (void) duration_s; (void) mean_acc_m; (void) observations;
}
void status_gnss_sys_note(const uint8_t *payload, uint16_t len) { (void) payload; (void) len; }

// --- bersaglio ----------------------------------------------------------------------
void bersaglio_avvio(void)
{
    gnss_fix_init();
    gnss_signal_init();
    gnss_ubx_ack_init();
    fuzz_imposta_max_len(4096);
}

void bersaglio_semi(void)
{
    static const char *const flussi[] = {
        "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,*43\r\n"
        "$GNRMC,101010.00,A,4100.0000000,N,01630.0000000,E,0.01,0.0,091026,,,A,V*07\r\n"
        "$GNGSV,1,1,01,05,45,120,40,1*48\r\n",
        "#PPPNAVA,COM1,0,80.0,FINE,2400,123456.000,0,0,18,0;SOL_COMPUTED,PPP_CONVERGING,41.00000000000,"
        "16.50000000000,409.5000,40.5000,WGS84,0.1234,0.2345,0.3456,\"0\",5.000,0.000,30,25,0,0,0,0,0,0*00000000\r\n"
        "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,*43\r\n"
        "$GNRMC,101010.00,A,4100.0000000,N,01630.0000000,E,0.01,0.0,091026,,,A,V*07\r\n",
        "#INSPVAXA,COM1,0,73.5,FINESTEERING,2400,123456.000,0,0,0;INS_SOLUTION_GOOD,INS_RTKFIXED,41.0,16.5,450.0,"
        "40.5,0.01,0.02,0.00,1.5,-0.5,123.4,0.01,0.01,0.02,0.1,0.1,0.2,0,0*00000000\r\n"
        "#HEADINGA,COM1,0,73.5,FINESTEERING,2400,123456.000,0,0,0;SOL_COMPUTED,NARROW_INT,1.234,123.45,-0.5,0,0.1,0.2,"
        "\"0\",20,18,18,18,0,0,0,0*00000000\r\n"
        "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,4,10,0.9,409.5000,M,40.500,M,1.0,0000*00\r\n",
    };
    static const uint8_t opz[] = { 0x13, 0x03, 0x11 };
    for (size_t i = 0; i < sizeof(flussi) / sizeof(flussi[0]); i++) {
        static uint8_t b[2048];
        size_t n = strlen(flussi[i]);
        for (int k = 0; k < 2; k++) {
            b[0] = (uint8_t) (opz[i] ^ (k ? 0x04 : 0));
            memcpy(b + 1, flussi[i], n);
            fuzz_seme(b, n + 1);
        }
    }
    static const uint8_t ack[] = { 0x19, 0xB5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x8A, 0x98, 0xC1, '$', 'G' };
    fuzz_seme(ack, sizeof(ack));
    static const char *const parole[] = { "$GNGGA,", "$GNRMC,", "$GPGSV,", "#PPPNAVA,", "#BESTPOSA,", "#INSPVAXA,", "#HEADINGA,",
                                          "#VERSIONA,", ",A,", ",N,", ",E,", ";", "\r\n", "*" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

static void prove_dirette(const char *riga)
{
    size_t len = strlen(riga);
    if (len >= LINE_BUF_SIZE) len = LINE_BUF_SIZE - 1;
    // riga che finisce contro la pagina protetta
    char *s = fuzz_stringa(riga, len);
    if (s[0] == '$' && len > 6 && memcmp(&s[3], "RMC", 3) == 0) {
        // BUG NOTO (test_gnss_nmea_reader.c, rmc_con_data_corta_ignorata):
        // nmea_rmc_time legge fino a 5 byte oltre la fine della riga. Finche'
        // non e' corretto si lasciano 6 byte a zero dopo il '\0', altrimenti
        // ogni giro si ferma qui: dopo la correzione mettere il margine a 0.
#ifndef FUZZ_RMC_MARGINE
#define FUZZ_RMC_MARGINE 6
#endif
        char *r = (char *) fuzz_buffer(len + 1 + FUZZ_RMC_MARGINE);
        memset(r, 0, len + 1 + FUZZ_RMC_MARGINE);
        memcpy(r, s, len);
        nmea_rmc_time(r);
    }
    char out[24];
    size_t cap = 1 + fuzz_caso() % sizeof(out);
    char *o = (char *) fuzz_buffer(cap);
    (void) bynav_data_field(s, (int) (fuzz_caso() % 24), o, cap);
    handle_bynav_line(s);
    // riscrittura HAS nel buffer di LINE_BUF_SIZE byte, come nel task
    if (s[0] == '$' && len > 6) {
        char *l = (char *) fuzz_buffer(LINE_BUF_SIZE);
        memcpy(l, s, len + 1);
        size_t ll = len;
        has_fallback_line(l, &ll);
        FUZZ_VERIFICA(ll == strlen(l) && ll < LINE_BUF_SIZE);
    }
    fuzz_zone_azzera();
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    g_opz = d[0];
    uint8_t *f = aiuto_copia(d + 1, n - 1);
    n--;
    if (g_opz & 1) aiuto_aggiusta_flusso(f, n);
    // stato da capo
    s_convert = false;
    s_use_ppp = false;
    s_h_ell_m = 0;
    s_has_state = "";
    memset(&s_ppp, 0, sizeof(s_ppp));
    (void) gnss_ubx_ack_wait(0, NULL, NULL, NULL); // chiude la finestra UBX
    for (int k = 0; k < 600 && gnss_ubx_ack_feed_byte(0); k++) {
    }
    if (g_opz & 8) gnss_ubx_ack_arm();
    g_stub_impostazioni.rover_has_fallback = (g_opz & 2) != 0;

    // 1) il task vero sul flusso
    g_flusso = f;
    g_flusso_n = n;
    g_flusso_i = 0;
    if (!setjmp(g_fine)) {
        gnss_nmea_reader_task(NULL);
    }

    // 2) le funzioni interne riga per riga, contro la pagina protetta
    size_t i = 0;
    while (i < n) {
        size_t j = i;
        while (j < n && f[j] != '\n' && f[j] != '\r') j++;
        if (j > i) {
            char riga[LINE_BUF_SIZE];
            size_t k = j - i < sizeof(riga) - 1 ? j - i : sizeof(riga) - 1;
            memcpy(riga, f + i, k);
            riga[k] = '\0';
            prove_dirette(riga);
        }
        i = j + 1;
    }
}
