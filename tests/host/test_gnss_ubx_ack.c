// Prove di main/gnss_ubx_ack.c: riconoscimento dei frame UBX (ACK/NAK,
// MON-VER, NAV-SVIN, NAV-TIMEUTC) nello stream del ricevitore u-blox.
// Frame ACK/NAK calcolati in Python (tests/host/strumenti/riferimenti.py,
// sezione "UBX"); gli altri costruiti qui con il checksum Fletcher.
#include "prove.h"
#include "../../main/gnss_ubx_ack.c"

// --- sostituti di status.c / time_sync.c ----------------------------------
static int s_svin_n, s_time_n;
static bool s_svin_active, s_svin_valid;
static uint32_t s_svin_dur, s_svin_obs;
static float s_svin_acc;
static int s_t[6];

void status_svin_note(bool active, bool valid, uint32_t duration_s, float mean_acc_m, uint32_t observations)
{
    s_svin_n++;
    s_svin_active = active;
    s_svin_valid = valid;
    s_svin_dur = duration_s;
    s_svin_acc = mean_acc_m;
    s_svin_obs = observations;
}

void status_gnss_sys_note(const uint8_t *payload, uint16_t len) { (void) payload; (void) len; }

void time_sync_from_gnss(int year, int month, int day, int hour, int min, int sec)
{
    s_time_n++;
    s_t[0] = year; s_t[1] = month; s_t[2] = day; s_t[3] = hour; s_t[4] = min; s_t[5] = sec;
}

static const uint8_t ACK[] = { 0xB5, 0x62, 0x05, 0x01, 0x02, 0x00, 0x06, 0x8A, 0x98, 0xC1 };
static const uint8_t NAK[] = { 0xB5, 0x62, 0x05, 0x00, 0x02, 0x00, 0x06, 0x8A, 0x97, 0xBC };

static uint8_t s_frame[600];

static size_t costruisci(uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t n)
{
    uint8_t *f = s_frame;
    f[0] = 0xB5; f[1] = 0x62; f[2] = cls; f[3] = id; f[4] = (uint8_t) n; f[5] = (uint8_t) (n >> 8);
    memcpy(f + 6, pl, n);
    uint8_t a = 0, b = 0;
    for (size_t i = 2; i < 6u + n; i++) { a += f[i]; b += a; }
    f[6 + n] = a;
    f[7 + n] = b;
    return 8u + n;
}

static int passa(const uint8_t *f, size_t n)
{
    int consumati = 0;
    for (size_t i = 0; i < n; i++) consumati += gnss_ubx_ack_feed_byte(f[i]);
    return consumati;
}

static void azzera(void)
{
    gnss_ubx_ack_init();
    s_state = ST_IDLE;
    s_armed = false;
    s_model[0] = s_fw[0] = '\0';
    s_svin_n = s_time_n = 0;
}

PROVA(ack_riconosciuto_con_classe_e_id)
{
    azzera();
    gnss_ubx_ack_arm();
    VERIFICA_INT(passa(ACK, sizeof(ACK)), sizeof(ACK));
    bool ok = false;
    uint8_t cls = 0, id = 0;
    VERIFICA(gnss_ubx_ack_wait(10, &ok, &cls, &id));
    VERIFICA(ok);
    VERIFICA_INT(cls, 0x06);
    VERIFICA_INT(id, 0x8A);
}

PROVA(nak_riconosciuto)
{
    azzera();
    gnss_ubx_ack_arm();
    passa(NAK, sizeof(NAK));
    bool ok = true;
    VERIFICA(gnss_ubx_ack_wait(10, &ok, NULL, NULL));
    VERIFICA(!ok);
}

PROVA(ack_con_checksum_sbagliato_ignorato)
{
    azzera();
    gnss_ubx_ack_arm();
    uint8_t f[sizeof(ACK)];
    memcpy(f, ACK, sizeof(f));
    f[sizeof(f) - 1] ^= 0x01;
    passa(f, sizeof(f));
    bool ok;
    VERIFICA(!gnss_ubx_ack_wait(10, &ok, NULL, NULL));
}

PROVA(senza_arm_nessun_byte_consumato)
{
    // Fuori dalla finestra di attesa lo stream (RTCM della base) non si tocca.
    azzera();
    VERIFICA_INT(passa(ACK, sizeof(ACK)), 0);
    gnss_ubx_ack_arm();
    VERIFICA_INT(passa(ACK, sizeof(ACK)), sizeof(ACK));
    VERIFICA(gnss_ubx_ack_wait(10, NULL, NULL, NULL));
    VERIFICA_INT(passa(ACK, sizeof(ACK)), 0); // finestra chiusa da wait
}

PROVA(b5_isolato_restituisce_il_byte_seguente)
{
    azzera();
    gnss_ubx_ack_arm();
    VERIFICA(gnss_ubx_ack_feed_byte(0xB5));
    VERIFICA(!gnss_ubx_ack_feed_byte('$')); // va al lettore NMEA
    VERIFICA_INT(passa(ACK, sizeof(ACK)), sizeof(ACK));
}

// MON-VER: swVersion[30] + hwVersion[10] + n estensioni da 30 byte
static uint16_t mon_ver(uint8_t *pl, int n_est, int pos_mod)
{
    memset(pl, 0, 40 + 30 * n_est);
    strcpy((char *) pl, "EXT CORE 1.00 (prova)");
    strcpy((char *) pl + 30, "00190000");
    for (int i = 0; i < n_est; i++) {
        char *e = (char *) pl + 40 + 30 * i;
        if (i == pos_mod) strcpy(e, "MOD=ZED-F9P");
        else if (i == 1) strcpy(e, "FWVER=HPG 1.32");
        else strcpy(e, "PROTVER=27.31");
    }
    return (uint16_t) (40 + 30 * n_est);
}

PROVA(mon_ver_con_8_estensioni_letto)
{
    azzera();
    gnss_ubx_ack_arm();
    uint8_t pl[600];
    uint16_t n = mon_ver(pl, 8, 7);
    passa(s_frame, costruisci(0x0A, 0x04, pl, n));
    VERIFICA_STR(gnss_ubx_ack_model(), "ZED-F9P");
    VERIFICA_STR(gnss_ubx_ack_fw(), "HPG 1.32");
}

PROVA(mon_ver_con_12_estensioni_400_byte_letto)
{
    azzera();
    gnss_ubx_ack_arm();
    uint8_t pl[600];
    uint16_t n = mon_ver(pl, 12, 11); // modello nell'ultima riga
    VERIFICA_INT(n, 400);
    passa(s_frame, costruisci(0x0A, 0x04, pl, n));
    VERIFICA_STR(gnss_ubx_ack_model(), "ZED-F9P");
}

PROVA(mon_ver_oltre_400_byte_scartato_senza_danni)
{
    // 13 estensioni: oltre il buffer, il frame si lascia perdere (limite
    // voluto, vedi commento di s_payload). Poi un ACK si legge ancora.
    azzera();
    gnss_ubx_ack_arm();
    uint8_t pl[600];
    uint16_t n = mon_ver(pl, 13, 0);
    passa(s_frame, costruisci(0x0A, 0x04, pl, n));
    VERIFICA_STR(gnss_ubx_ack_model(), "");
    passa(ACK, sizeof(ACK));
    VERIFICA(gnss_ubx_ack_wait(10, NULL, NULL, NULL));
}

PROVA(mon_ver_estensione_lunga_30_caratteri_senza_terminatore)
{
    azzera();
    gnss_ubx_ack_arm();
    uint8_t pl[600];
    uint16_t n = mon_ver(pl, 3, 0);
    memcpy(pl + 40, "MOD=ABCDEFGHIJKLMNOPQRSTUVWXYZ", 30); // 30 byte pieni
    passa(s_frame, costruisci(0x0A, 0x04, pl, n));
    // il modello si tronca alla dimensione del campo (23 caratteri), senza sforare
    VERIFICA_STR(gnss_ubx_ack_model(), "ABCDEFGHIJKLMNOPQRSTUVW");
}

PROVA(nav_svin_campi_letti)
{
    azzera();
    gnss_ubx_ack_arm();
    uint8_t pl[40] = { 0 };
    uint32_t dur = 345, acc = 12345, obs = 678; // acc in 0,1 mm -> 1.2345 m
    memcpy(pl + 8, &dur, 4);
    memcpy(pl + 28, &acc, 4);
    memcpy(pl + 32, &obs, 4);
    pl[36] = 1; // valid
    pl[37] = 0; // active
    passa(s_frame, costruisci(0x01, 0x3B, pl, sizeof(pl)));
    VERIFICA_INT(s_svin_n, 1);
    VERIFICA_INT(s_svin_dur, 345);
    VERIFICA_INT(s_svin_obs, 678);
    VERIFICA_VICINO(s_svin_acc, 1.2345, 1e-5);
    VERIFICA(s_svin_valid);
    VERIFICA(!s_svin_active);
}

PROVA(nav_timeutc_valida_imposta_ora_non_valida_no)
{
    azzera();
    gnss_ubx_ack_arm();
    uint8_t pl[20] = { 0 };
    pl[12] = (uint8_t) (2026 & 0xFF); pl[13] = (uint8_t) (2026 >> 8);
    pl[14] = 10; pl[15] = 9; pl[16] = 13; pl[17] = 45; pl[18] = 30;
    pl[19] = 0x07; // validTOW, validWKN, validUTC
    passa(s_frame, costruisci(0x01, 0x21, pl, sizeof(pl)));
    VERIFICA_INT(s_time_n, 1);
    VERIFICA_INT(s_t[0], 2026);
    VERIFICA_INT(s_t[1], 10);
    VERIFICA_INT(s_t[2], 9);
    VERIFICA_INT(s_t[3], 13);
    VERIFICA_INT(s_t[4], 45);
    VERIFICA_INT(s_t[5], 30);
    pl[19] = 0x03; // senza validUTC
    passa(s_frame, costruisci(0x01, 0x21, pl, sizeof(pl)));
    VERIFICA_INT(s_time_n, 1);
}

int main(void)
{
    ESEGUI(ack_riconosciuto_con_classe_e_id);
    ESEGUI(nak_riconosciuto);
    ESEGUI(ack_con_checksum_sbagliato_ignorato);
    ESEGUI(senza_arm_nessun_byte_consumato);
    ESEGUI(b5_isolato_restituisce_il_byte_seguente);
    ESEGUI(mon_ver_con_8_estensioni_letto);
    ESEGUI(mon_ver_con_12_estensioni_400_byte_letto);
    ESEGUI(mon_ver_oltre_400_byte_scartato_senza_danni);
    ESEGUI(mon_ver_estensione_lunga_30_caratteri_senza_terminatore);
    ESEGUI(nav_svin_campi_letti);
    ESEGUI(nav_timeutc_valida_imposta_ora_non_valida_no);
    return prove_fine();
}
