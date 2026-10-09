// Prove di main/gnss_unicore.c: controllo dei log ASCII Unicore/NovAtel
// (CRC32 di #PPPNAVA / #BESTPOSA) e lettura dei campi della soluzione PPP
// (Galileo HAS) e di #VERSIONA.
//
// Righe e CRC32 calcolati in Python (tests/host/strumenti/riferimenti.py,
// sezione "Unicore": polinomio riflesso 0xEDB88320, valore iniziale 0,
// nessuna inversione finale). Intestazioni inventate, nessuna matricola.
#include "prove.h"
#include "../../main/gnss_unicore.c"

#include <stdio.h>

// --- sostituti dei moduli usati dalla configurazione (qui non provata) ----
int gnss_io_write(const void *data, size_t len) { (void) data; return (int) len; }
void settings_get_into(app_settings_t *out) { memset(out, 0, sizeof(*out)); }
void settings_peek(void (*fn)(const app_settings_t *s, void *ctx), void *ctx) { (void) fn; (void) ctx; }
uint8_t settings_nmea_rate_hz(void) { return 1; }
bool license_has(license_feature_t f) { (void) f; return true; }
bool base_measure_is_has(void) { return false; }
bool base_measure_is_active(void) { return false; }

static const char *PPP_OK =
    "#PPPNAVA,COM1,0,80.0,FINE,2400,123456.000,0,0,18,0;SOL_COMPUTED,PPP_CONVERGING,41.12345678901,"
    "16.98765432101,409.5000,40.5000,WGS84,0.1234,0.2345,0.3456,\"0\",5.000,0.000,30,25,0,0,0,0,0,0*8fa5f3fa";
static const char *PPP_NO_SOL =
    "#PPPNAVA,COM1,0,80.0,FINE,2400,123457.000,0,0,18,0;INSUFFICIENT_OBS,NONE,0.00000000000,0.00000000000,"
    "0.0000,0.0000,WGS84,0.0000,0.0000,0.0000,\"0\",0.000,0.000,5,0,0,0,0,0,0,0*b50bc17b";
static const char *BESTPOS =
    "#BESTPOSA,COM1,0,80.0,FINE,2400,123458.000,0,0,18,0;SOL_COMPUTED,PPP,-33.50000000000,-70.25000000000,"
    "500.0000,25.0000,WGS84,0.0500,0.0600,0.0900,\"0\",3.000,0.000,32,28,0,0,0,0,0,0*e558ee8e";
static const char *VERSION =
    "#VERSIONA,COM1,0,80.0,FINE,2400,123459.000,0,0,18,0;\"UM982\",\"R4.10Build99999\",\"COM1\",\"ffff\",\"2026/01/01\"*23abc4dc";

PROVA(crc32_come_calcolo_python)
{
    const char *star = strrchr(PPP_OK, '*');
    VERIFICA_INT(ascii_log_crc32(PPP_OK + 1, (size_t) (star - PPP_OK - 1)), 0x8fa5f3fau);
    star = strrchr(VERSION, '*');
    VERIFICA_INT(ascii_log_crc32(VERSION + 1, (size_t) (star - VERSION - 1)), 0x23abc4dcu);
}

PROVA(log_ascii_integro_accettato)
{
    VERIFICA(ascii_log_ok(PPP_OK));
    VERIFICA(ascii_log_ok(PPP_NO_SOL));
    VERIFICA(ascii_log_ok(BESTPOS));
}

PROVA(log_ascii_crc_maiuscolo_accettato)
{
    char r[400];
    strcpy(r, PPP_OK);
    char *s = strrchr(r, '*');
    for (char *p = s; *p; p++) if (*p >= 'a' && *p <= 'f') *p = (char) (*p - 32);
    VERIFICA(ascii_log_ok(r));
}

PROVA(log_ascii_corrotto_o_troncato_rifiutato)
{
    char r[400];
    strcpy(r, PPP_OK);
    r[90] = r[90] == '1' ? '2' : '1'; // una cifra cambiata
    VERIFICA(!ascii_log_ok(r));
    strcpy(r, PPP_OK);
    *strrchr(r, '*') = '\0'; // senza CRC
    VERIFICA(!ascii_log_ok(r));
    strcpy(r, PPP_OK);
    r[strlen(r) - 1] = '\0'; // CRC di 7 cifre
    VERIFICA(!ascii_log_ok(r));
    strcpy(r, PPP_OK);
    r[strlen(r) - 3] = 'x'; // cifra non esadecimale
    VERIFICA(!ascii_log_ok(r));
    VERIFICA(!ascii_log_ok("PPPNAVA senza cancelletto*8fa5f3fa"));
    VERIFICA(ascii_log_ok(PPP_OK)); // azzera il conteggio degli errori di fila
}

PROVA(pppnava_campi_letti)
{
    gnss_unicore_note_line(PPP_OK);
    gnss_unicore_ppp_t p;
    gnss_unicore_ppp_get(&p);
    VERIFICA(p.valid);
    VERIFICA_STR(p.type, "PPP_CONVERGING");
    VERIFICA_VICINO(p.lat_deg, 41.12345678901, 1e-12);
    VERIFICA_VICINO(p.lon_deg, 16.98765432101, 1e-12);
    VERIFICA_VICINO(p.h_ell_m, 450.0, 1e-9); // quota + ondulazione
    VERIFICA_VICINO(p.sig_lat_m, 0.1234, 1e-6);
    VERIFICA_VICINO(p.sig_lon_m, 0.2345, 1e-6);
    VERIFICA_VICINO(p.sig_h_m, 0.3456, 1e-6);
    VERIFICA_VICINO(p.corr_age_s, 5.0, 1e-6);
    VERIFICA_INT(p.sats_used, 25);
    VERIFICA(p.at_us > 0);
}

PROVA(pppnava_senza_soluzione_non_valida)
{
    gnss_unicore_note_line(PPP_NO_SOL);
    gnss_unicore_ppp_t p;
    gnss_unicore_ppp_get(&p);
    VERIFICA(!p.valid);
    VERIFICA_STR(p.type, "NONE");
}

PROVA(bestposa_ppp_emisferi_sud_ovest)
{
    gnss_unicore_note_line(BESTPOS);
    gnss_unicore_ppp_t p;
    gnss_unicore_ppp_get(&p);
    VERIFICA(p.valid);
    VERIFICA_VICINO(p.lat_deg, -33.5, 1e-12);
    VERIFICA_VICINO(p.lon_deg, -70.25, 1e-12);
    VERIFICA_VICINO(p.h_ell_m, 525.0, 1e-9);
    VERIFICA_INT(p.sats_used, 28);
}

PROVA(pppnava_corrotta_non_sostituisce_la_soluzione)
{
    gnss_unicore_note_line(PPP_OK);
    gnss_unicore_ppp_t prima;
    gnss_unicore_ppp_get(&prima);
    char r[400];
    strcpy(r, PPP_OK);
    char *lat = strstr(r, "41.123");
    lat[0] = '9'; // 91.123: cifra sbagliata, CRC originale
    gnss_unicore_note_line(r);
    gnss_unicore_ppp_t dopo;
    gnss_unicore_ppp_get(&dopo);
    VERIFICA_VICINO(dopo.lat_deg, prima.lat_deg, 0);
    VERIFICA_INT(dopo.at_us, prima.at_us);
    VERIFICA(ascii_log_ok(PPP_OK)); // azzera il conteggio degli errori di fila
}

PROVA(versiona_modello_e_firmware)
{
    gnss_unicore_note_line(VERSION);
    VERIFICA_STR(gnss_unicore_model(), "UM982");
    VERIFICA_STR(gnss_unicore_fw(), "R4.10Build99999");
}

PROVA(versiona_di_altro_costruttore_ignorata)
{
    gnss_unicore_note_line("#VERSIONA,COM1,0;\"K922\",\"V1.0\"*00000000");
    VERIFICA_STR(gnss_unicore_model(), "UM982"); // invariato
}

PROVA(diciannove_crc_sbagliati_poi_uno_giusto_controllo_resta_attivo)
{
    VERIFICA(ascii_log_ok(PPP_OK));
    char r[400];
    strcpy(r, PPP_OK);
    r[strlen(r) - 1] = r[strlen(r) - 1] == '0' ? '1' : '0';
    for (int i = 0; i < 19; i++) VERIFICA(!ascii_log_ok(r));
    VERIFICA(ascii_log_ok(PPP_OK));
    VERIFICA(!ascii_log_ok(r));
    VERIFICA(ascii_log_ok(PPP_OK));
}

// Va eseguita per ultima: spegne il controllo del CRC fino alla fine.
PROVA(venti_crc_sbagliati_di_fila_spengono_il_controllo)
{
    // Comportamento voluto (commento di ascii_log_ok): se la convenzione del
    // CRC fosse diversa su un modulo vero, dopo 20 righe di fila si accetta
    // la riga completa invece di perdere per sempre la soluzione HAS.
    char r[400];
    strcpy(r, PPP_OK);
    r[strlen(r) - 1] = r[strlen(r) - 1] == '0' ? '1' : '0';
    for (int i = 0; i < 19; i++) VERIFICA(!ascii_log_ok(r));
    VERIFICA(ascii_log_ok(r)); // la ventesima
    VERIFICA(ascii_log_ok(r));
    // senza "*xxxxxxxx" resta rifiutata anche cosi'
    char t[400];
    strcpy(t, PPP_OK);
    *strrchr(t, '*') = '\0';
    VERIFICA(!ascii_log_ok(t));
}

int main(void)
{
    ESEGUI(crc32_come_calcolo_python);
    ESEGUI(log_ascii_integro_accettato);
    ESEGUI(log_ascii_crc_maiuscolo_accettato);
    ESEGUI(log_ascii_corrotto_o_troncato_rifiutato);
    ESEGUI(pppnava_campi_letti);
    ESEGUI(pppnava_senza_soluzione_non_valida);
    ESEGUI(bestposa_ppp_emisferi_sud_ovest);
    ESEGUI(pppnava_corrotta_non_sostituisce_la_soluzione);
    ESEGUI(versiona_modello_e_firmware);
    ESEGUI(versiona_di_altro_costruttore_ignorata);
    ESEGUI(diciannove_crc_sbagliati_poi_uno_giusto_controllo_resta_attivo);
    ESEGUI(venti_crc_sbagliati_di_fila_spengono_il_controllo);
    return prove_fine();
}
