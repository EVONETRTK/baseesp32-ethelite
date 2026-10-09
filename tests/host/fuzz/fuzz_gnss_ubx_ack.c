// Fuzzing di main/gnss_ubx_ack.c (frame UBX nel flusso del ricevitore
// u-blox: ACK/NAK, MON-VER, NAV-SVIN, NAV-TIMEUTC, MON-SYS).
// Input: [opzioni][flusso]. Opzione bit 0: checksum Fletcher ricalcolato
// dopo la mutazione; bit 1: finestra di attesa aperta (arm) prima del flusso.
#include "motore.h"
#include "aiuti.h"
#include "../../../main/gnss_ubx_ack.c"

// --- sostituti di status.c / time_sync.c -----------------------------------
void status_svin_note(bool active, bool valid, uint32_t duration_s, float mean_acc_m, uint32_t observations)
{
    (void) active; (void) valid; (void) duration_s; (void) mean_acc_m; (void) observations;
}

void status_gnss_sys_note(const uint8_t *payload, uint16_t len)
{
    // il vero status.c legge il payload fino a len: deve stare nel buffer
    FUZZ_VERIFICA(len <= sizeof(s_payload));
    volatile uint8_t x = 0;
    for (uint16_t i = 0; i < len && i < sizeof(s_payload); i++) x ^= payload[i];
}

void time_sync_from_gnss(int year, int month, int day, int hour, int min, int sec)
{
    (void) year; (void) month; (void) day; (void) hour; (void) min; (void) sec;
}

void bersaglio_avvio(void)
{
    gnss_ubx_ack_init();
    fuzz_imposta_max_len(3000);
}

static size_t frame(uint8_t *f, uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t n)
{
    f[0] = 0xB5; f[1] = 0x62; f[2] = cls; f[3] = id; f[4] = (uint8_t) n; f[5] = (uint8_t) (n >> 8);
    memcpy(f + 6, pl, n);
    aiuto_ubx_ck(f + 2, 4u + n, &f[6 + n], &f[7 + n]);
    return 8u + n;
}

void bersaglio_semi(void)
{
    static uint8_t b[2000], pl[600];
    size_t n = 0;
    b[n++] = 3;
    const uint8_t ack[] = { 0x06, 0x8A };
    n += frame(b + n, 0x05, 0x01, ack, 2);
    n += frame(b + n, 0x05, 0x00, ack, 2);
    memset(pl, 0, sizeof(pl));
    memcpy(pl, "EXT CORE 1.00 (61b2dd)", 22);
    memcpy(pl + 30, "00190000", 8);
    const char *ext[] = { "ROM BASE 0x118B2060", "FWVER=HPG 1.32", "PROTVER=27.31", "MOD=ZED-F9P", "GPS;GLO;GAL;BDS",
                          "SBAS;QZSS", "NAVIC" };
    for (int i = 0; i < 7; i++) memcpy(pl + 40 + 30 * i, ext[i], strlen(ext[i]));
    n += frame(b + n, 0x0A, 0x04, pl, 40 + 30 * 7);
    memset(pl, 0, 40);
    pl[8] = 60; pl[28] = 0x10; pl[29] = 0x27; pl[32] = 61; pl[36] = 1; pl[37] = 0;
    n += frame(b + n, 0x01, 0x3B, pl, 40);
    memset(pl, 0, 20);
    pl[12] = 0xEA; pl[13] = 0x07; pl[14] = 10; pl[15] = 9; pl[16] = 12; pl[17] = 30; pl[18] = 5; pl[19] = 0x07;
    n += frame(b + n, 0x01, 0x21, pl, 20);
    memset(pl, 0, 24);
    n += frame(b + n, 0x0A, 0x39, pl, 24);
    fuzz_seme(b, n);
    b[0] = 2; // senza ricalcolo
    fuzz_seme(b, n);
    fuzz_parola_bin("\xB5\x62", 2);
    fuzz_parola_bin("\xB5\x62\x0A\x04", 4);
    fuzz_parola_bin("\xB5\x62\x01\x3B", 4);
    fuzz_parola_bin("\xB5\x62\x01\x21", 4);
    fuzz_parola_bin("\xB5\x62\x05\x01\x02\x00", 6);
    fuzz_parola("MOD=");
    fuzz_parola("FWVER=");
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    uint8_t opz = d[0];
    uint8_t *f = aiuto_copia(d + 1, n - 1);
    n--;
    if (opz & 1) aiuto_aggiusta_flusso(f, n);
    s_state = ST_IDLE;
    if (opz & 2) gnss_ubx_ack_arm();
    uint8_t *b = fuzz_buffer(n);
    memcpy(b, f, n);
    for (size_t i = 0; i < n; i++) (void) gnss_ubx_ack_feed_byte(b[i]);
    bool acked;
    uint8_t c, id;
    (void) gnss_ubx_ack_wait(0, &acked, &c, &id);
    FUZZ_VERIFICA(strlen(gnss_ubx_ack_model()) < sizeof(s_model));
    FUZZ_VERIFICA(strlen(gnss_ubx_ack_fw()) < sizeof(s_fw));
}
