// Prove di main/gnss_nmea_reader.c: lettura dell'ora dalla riga RMC.
// (Il resto del task e' provato dal fuzzing: tests/host/fuzz.ps1.)
#include "prove.h"
#include "../../main/etrf.c"
#include "../../main/nmea_etrf.c"
#include "../../main/gnss_nmea_reader.c"

#include <stdio.h>

// --- sostituti dei moduli usati dal task (qui non avviato) ---------------------
static int s_ora_n;
static int s_ora[6];
void time_sync_from_gnss(int year, int month, int day, int hour, int min, int sec)
{
    s_ora_n++;
    s_ora[0] = year; s_ora[1] = month; s_ora[2] = day; s_ora[3] = hour; s_ora[4] = min; s_ora[5] = sec;
}
bool time_sync_wants_gnss(void) { return true; }
int gnss_io_read(uint8_t *buf, size_t len, TickType_t timeout) { (void) buf; (void) len; (void) timeout; return 0; }
void sys_stats_heartbeat(heartbeat_t which) { (void) which; }
void nmea_udp_broadcast_send(const char *line, size_t len) { (void) line; (void) len; }
bool gnss_ubx_ack_feed_byte(uint8_t c) { (void) c; return false; }
void gnss_fix_parse_gga(const char *line) { (void) line; }
bool nmea_checksum_ok(const char *line) { (void) line; return true; }
void gnss_signal_parse_gsv(const char *line) { (void) line; }
void ntrip_rover_client_forward_gga(const char *line, size_t len) { (void) line; (void) len; }
void gnss_unicore_note_line(const char *line) { (void) line; }
void gnss_comnav_note_line(const char *line) { (void) line; }
void gnss_unicore_ppp_get(gnss_unicore_ppp_t *out) { memset(out, 0, sizeof(*out)); }
int64_t status_get_last_rtcm_time_us(void) { return 0; }
void status_bynav_ins_note_inspvaxa(const char *s, float r, float p, float h) { (void) s; (void) r; (void) p; (void) h; }
void status_bynav_ins_note_headinga(float h, float p) { (void) h; (void) p; }
bool license_has(license_feature_t f) { (void) f; return true; }
void settings_peek(void (*fn)(const app_settings_t *s, void *ctx), void *ctx) { (void) fn; (void) ctx; }

PROVA(ora_e_data_della_rmc_lette)
{
    s_ora_n = 0;
    nmea_rmc_time("$GNRMC,101010.00,A,4100.0000000,N,01630.0000000,E,0.01,0.0,091026,,,A,V*07");
    VERIFICA_INT(s_ora_n, 1);
    VERIFICA_INT(s_ora[0], 2026);
    VERIFICA_INT(s_ora[1], 10);
    VERIFICA_INT(s_ora[2], 9);
    VERIFICA_INT(s_ora[3], 10);
    VERIFICA_INT(s_ora[4], 10);
    VERIFICA_INT(s_ora[5], 10);
}

// Trovato dal fuzzing (bersaglio gnss_nmea_reader): con il campo della data
// (o dell'ora) piu' corto di 6 caratteri, nmea_rmc_time legge f[8][0..5]
// anche oltre la fine della riga ('\0' compreso): qui i byte "1026" rimasti
// nel buffer dopo il terminatore finiscono nella data. Nel task il buffer e'
// la riga di 320 byte sullo stack: si leggono resti delle righe precedenti
// (oltre il buffer solo con una riga di 316+ caratteri), e con cifre giuste
// per caso l'orologio della scheda viene messo a una data sbagliata
// (time_sync_from_gnss non rifiuta nemmeno ore/minuti/secondi negativi).
// Una RMC cosi' arriva solo con il checksum giusto per caso (1 su 256) o con
// un '*' presto nella riga. Correzione proposta: prima di leggere, verificare
// che i 6 caratteri di f[0] e di f[8] siano cifre (controllo in ordine con
// &&, che si ferma al primo non-cifra, '\0' compreso); in time_sync_from_gnss
// rifiutare anche hour/min/sec < 0.
PROVA(rmc_con_data_corta_ignorata)
{
    char buf[64];
    memset(buf, 0, sizeof(buf));
    const char *riga = "$GPRMC,120000,A,0,N,0,E,0,0,09";
    strcpy(buf, riga);
    memcpy(buf + strlen(riga) + 1, "1026", 4); // resti di una riga precedente
    s_ora_n = 0;
    nmea_rmc_time(buf);
    VERIFICA_INT(s_ora_n, 0); // data incompleta: l'ora non va presa
    // ora di 2 caratteri
    s_ora_n = 0;
    nmea_rmc_time("$GPRMC,1,A,0,N,0,E,0,0,091026");
    VERIFICA_INT(s_ora_n, 0);
}

int main(void)
{
    ESEGUI(ora_e_data_della_rmc_lette);
    ESEGUI(rmc_con_data_corta_ignorata);
    return prove_fine();
}
