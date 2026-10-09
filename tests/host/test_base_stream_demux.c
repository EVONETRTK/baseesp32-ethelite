// Prove di main/base_stream_demux.c: separazione del flusso della base in
// frame RTCM3 (verso il caster), righe NMEA/ASCII e frame UBX.
//
// Usa il vero gnss_fix.c (la GGA aggiorna lo stato del fix); GSV e righe
// '#' finiscono in sostituti che le registrano. Frame RTCM costruiti in
// Python (tests/host/strumenti/riferimenti.py, sezione "RTCM").
#include "prove.h"
#include "../../main/gnss_fix.c"
#include "../../main/base_stream_demux.c"

#include <stdio.h>

// --- sostituti dei moduli chiamati dal demultiplexer ---------------------
static int s_gsv, s_unicore, s_comnav;
static char s_ultima_hash[300];

void gnss_signal_parse_gsv(const char *line) { (void) line; s_gsv++; }
void gnss_unicore_note_line(const char *line) { s_unicore++; strlcpy(s_ultima_hash, line, sizeof(s_ultima_hash)); }
void gnss_comnav_note_line(const char *line) { (void) line; s_comnav++; }

// --- frame RTCM ricevuti dal caster ----------------------------------------
static int s_frame;
static uint16_t s_tipi[64];

static void su_rtcm(const uint8_t *f, size_t len)
{
    VERIFICA(len >= 6);
    if (s_frame < 64) s_tipi[s_frame] = (uint16_t) ((f[3] << 4) | (f[4] >> 4));
    s_frame++;
}

static const uint8_t F1005[] = {
    0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x03, 0x8A, 0xC5, 0x4F, 0x99, 0xD2, 0x03,
    0x30, 0x95, 0x7F, 0x2E, 0x09, 0xB0, 0xBE, 0xF0, 0x34, 0xF6, 0x3D, 0x42,
};
static const uint8_t F1077[] = {
    0xD3, 0x00, 0x09, 0x43, 0x51, 0x23, 0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62, 0x0F, 0xA7, 0xF9,
};
static const char *GGA = "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57\r\n";
static const char *GSV = "$GNGSV,1,1,01,05,45,120,40,1*48\r\n";
static const char *PPP =
    "#PPPNAVA,COM1,0,80.0,FINE,2400,123456.000,0,0,18,0;SOL_COMPUTED,PPP_CONVERGING,41.12345678901,"
    "16.98765432101,409.5000,40.5000,WGS84,0.1234,0.2345,0.3456,\"0\",5.000,0.000,30,25,0,0,0,0,0,0*8fa5f3fa\r\n";

// --- costruzione del flusso ---------------------------------------------------
static uint8_t s_buf[4096];
static size_t s_len;

static void metti(const void *p, size_t n) { memcpy(s_buf + s_len, p, n); s_len += n; }
static void metti_str(const char *s) { metti(s, strlen(s)); }

// UBX con payload dato (checksum Fletcher calcolato qui)
static void metti_ubx(uint8_t cls, uint8_t id, const uint8_t *pl, uint16_t n)
{
    uint8_t h[6] = { 0xB5, 0x62, cls, id, (uint8_t) n, (uint8_t) (n >> 8) };
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6; i++) { a += h[i]; b += a; }
    for (int i = 0; i < n; i++) { a += pl[i]; b += a; }
    metti(h, 6);
    metti(pl, n);
    metti(&a, 1);
    metti(&b, 1);
}

static void azzera(void)
{
    gnss_fix_init();
    s_state = ST_IDLE;
    s_gsv = s_unicore = s_comnav = s_frame = 0;
    s_len = 0;
    memset(s_tipi, 0, sizeof(s_tipi));
}

static void passa_a_pezzi(size_t pezzo)
{
    for (size_t i = 0; i < s_len; i += pezzo) {
        size_t n = s_len - i < pezzo ? s_len - i : pezzo;
        base_stream_demux_feed(s_buf + i, n, su_rtcm);
    }
}

static void flusso_misto(void)
{
    const uint8_t ack[] = { 0x06, 0x8A };
    metti_str(GGA);
    metti(F1005, sizeof(F1005));
    metti_ubx(0x05, 0x01, ack, 2);
    metti_str(GSV);
    metti(F1077, sizeof(F1077));
    metti_str(PPP);
    metti(F1005, sizeof(F1005));
}

PROVA(flusso_misto_tutto_separato)
{
    azzera();
    flusso_misto();
    passa_a_pezzi(s_len);
    VERIFICA_INT(s_frame, 3);
    VERIFICA_INT(s_tipi[0], 1005);
    VERIFICA_INT(s_tipi[1], 1077);
    VERIFICA_INT(s_tipi[2], 1005);
    VERIFICA_INT(s_gsv, 1);
    VERIFICA_INT(s_unicore, 1);
    VERIFICA_INT(s_comnav, 1);
    VERIFICA(strncmp(s_ultima_hash, "#PPPNAVA,", 9) == 0);
    VERIFICA(strchr(s_ultima_hash, '\r') == NULL);
    gnss_fix_status_t fx = gnss_fix_get_status();
    VERIFICA(fx.valid);
    VERIFICA_INT(fx.quality, GNSS_FIX_RTK_FIXED);
}

PROVA(flusso_misto_a_frammenti_di_ogni_misura)
{
    for (size_t pezzo = 1; pezzo <= 40; pezzo++) {
        azzera();
        flusso_misto();
        passa_a_pezzi(pezzo);
        if (s_frame != 3 || s_gsv != 1 || s_unicore != 1 || !gnss_fix_get_status().valid) {
            prove_errore(__FILE__, __LINE__, "frammenti da %u byte: %d frame, %d GSV, %d righe #",
                         (unsigned) pezzo, s_frame, s_gsv, s_unicore);
        }
    }
}

PROVA(byte_spuri_tra_i_messaggi_ignorati)
{
    azzera();
    const uint8_t spuri[] = { 0x00, 0xFF, 0x0A, 0x0D, 0xB5, 0x41, 0x7E, 0x80, 0xB5 };
    metti(spuri, sizeof(spuri));
    metti(F1005, sizeof(F1005));
    metti(spuri, sizeof(spuri));
    metti_str(GSV);
    metti(spuri, sizeof(spuri));
    metti(F1077, sizeof(F1077));
    passa_a_pezzi(s_len);
    VERIFICA_INT(s_frame, 2);
    VERIFICA_INT(s_gsv, 1);
}

PROVA(falso_d3_con_lunghezza_lunga_non_fa_perdere_il_frame_vero)
{
    // 0xD3 qualunque con lunghezza 64: si "mangerebbe" il 1005 che segue.
    azzera();
    const uint8_t falso[] = { 0xD3, 0x00, 0x40 };
    metti(falso, sizeof(falso));
    metti(F1005, sizeof(F1005));
    metti_str(GSV);
    for (int i = 0; i < 60; i++) metti_str(" "); // riempitivo oltre la lunghezza dichiarata
    metti(F1077, sizeof(F1077));
    passa_a_pezzi(7);
    VERIFICA_INT(s_frame, 2);
    VERIFICA_INT(s_tipi[0], 1005);
    VERIFICA_INT(s_tipi[1], 1077);
    VERIFICA_INT(s_gsv, 1);
}

PROVA(falso_d3_con_bit_riservati_scartato_subito)
{
    azzera();
    const uint8_t falso[] = { 0xD3, 0xFC, 0xD3 };
    metti(falso, sizeof(falso)); // 0xFC: bit riservati non a zero, poi un altro 0xD3 falso
    metti(F1005, sizeof(F1005));
    passa_a_pezzi(s_len);
    VERIFICA_INT(s_frame, 1);
}

PROVA(rtcm_con_crc_sbagliato_scartato_e_il_seguente_passa)
{
    azzera();
    uint8_t rotto[sizeof(F1005)];
    memcpy(rotto, F1005, sizeof(rotto));
    rotto[10] ^= 0x04;
    metti(rotto, sizeof(rotto));
    metti(F1077, sizeof(F1077));
    metti(F1005, sizeof(F1005));
    // Il frame rotto contiene "D3 03 8A" (ID stazione + inizio di X): nella
    // rianalisi sembra un frame da 906 byte, e i due frame seguenti escono
    // solo quando ne sono arrivati altri ~900 (qui righe GSV). Ritardo, non
    // perdita: si controlla che alla fine escano tutti, in ordine.
    for (int i = 0; i < 30; i++) metti_str(GSV);
    passa_a_pezzi(s_len);
    VERIFICA_INT(s_frame, 2);
    VERIFICA_INT(s_tipi[0], 1077);
    VERIFICA_INT(s_tipi[1], 1005);
    VERIFICA_INT(s_gsv, 30);
}

PROVA(riga_interrotta_da_frame_rtcm)
{
    azzera();
    metti_str("$GNGGA,123519.00,4807.03"); // riga troncata, senza fine riga
    metti(F1005, sizeof(F1005));
    metti_str("$GNGSV,1,1,0"); // interrotta dall'inizio di un'altra riga
    metti_str(GSV);
    passa_a_pezzi(s_len);
    VERIFICA_INT(s_frame, 1);
    VERIFICA_INT(s_gsv, 1);
    VERIFICA(!gnss_fix_get_status().valid); // la GGA troncata non e' stata letta
}

PROVA(nmea_con_checksum_sbagliato_non_letta)
{
    azzera();
    metti_str("$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*58\r\n");
    metti_str("$GNGSV,1,1,01,05,45,120,40,1*49\r\n");
    passa_a_pezzi(s_len);
    VERIFICA(!gnss_fix_get_status().valid);
    VERIFICA_INT(s_gsv, 0);
}

PROVA(frame_ubx_saltato_per_intero_anche_con_rtcm_dentro)
{
    // Un RXM-RAWX lungo con dentro byte che sembrano RTCM e NMEA: nulla
    // deve uscire dal payload UBX.
    azzera();
    uint8_t pl[3000];
    for (size_t i = 0; i < sizeof(pl); i++) pl[i] = (uint8_t) (i * 7);
    memcpy(pl + 100, F1077, sizeof(F1077));
    memcpy(pl + 500, "$GNGSV,1,1,01,05,45,120,40,1*48\r\n", 33);
    metti_ubx(0x02, 0x15, pl, sizeof(pl));
    metti(F1005, sizeof(F1005));
    passa_a_pezzi(64);
    VERIFICA_INT(s_frame, 1);
    VERIFICA_INT(s_tipi[0], 1005);
    VERIFICA_INT(s_gsv, 0);
}

PROVA(b5_isolato_non_fa_perdere_la_riga)
{
    azzera();
    const uint8_t b5 = 0xB5;
    metti(&b5, 1);
    metti_str(GSV);
    metti(&b5, 1);
    metti(F1005, sizeof(F1005));
    passa_a_pezzi(s_len);
    VERIFICA_INT(s_gsv, 1);
    VERIFICA_INT(s_frame, 1);
}

int main(void)
{
    ESEGUI(flusso_misto_tutto_separato);
    ESEGUI(flusso_misto_a_frammenti_di_ogni_misura);
    ESEGUI(byte_spuri_tra_i_messaggi_ignorati);
    ESEGUI(falso_d3_con_lunghezza_lunga_non_fa_perdere_il_frame_vero);
    ESEGUI(falso_d3_con_bit_riservati_scartato_subito);
    ESEGUI(rtcm_con_crc_sbagliato_scartato_e_il_seguente_passa);
    ESEGUI(riga_interrotta_da_frame_rtcm);
    ESEGUI(nmea_con_checksum_sbagliato_non_letta);
    ESEGUI(frame_ubx_saltato_per_intero_anche_con_rtcm_dentro);
    ESEGUI(b5_isolato_non_fa_perdere_la_riga);
    return prove_fine();
}
