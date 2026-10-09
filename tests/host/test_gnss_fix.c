// Prove di main/gnss_fix.c: checksum NMEA e lettura della GGA (stato del
// fix, posizione usata anche dalla misura della posizione della base).
// Checksum attesi calcolati in Python (tests/host/strumenti/riferimenti.py).
#include "prove.h"
#include "../../main/gnss_fix.c"

#include <stdio.h>

// GGA di riferimento (RTK fisso, 48 07.038 N, 011 31.000 E)
static const char *GGA_FISSO =
    "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57";
static const char *GGA_FLOAT =
    "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,5,12,0.8,545.4,M,46.9,M,2.0,0000*55";
static const char *GGA_SUD_OVEST =
    "$GPGGA,000001.00,3352.1234567,S,07038.7654321,W,1,08,1.2,12.3,M,-25.1,M,,*79";
static const char *GGA_VUOTA = "$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5D";

static void azzera(void)
{
    gnss_fix_init();
}

PROVA(checksum_giusto_accettato)
{
    VERIFICA(nmea_checksum_ok(GGA_FISSO));
    VERIFICA(nmea_checksum_ok(GGA_FLOAT));
    VERIFICA(nmea_checksum_ok(GGA_SUD_OVEST));
    VERIFICA(nmea_checksum_ok(GGA_VUOTA));
    VERIFICA(nmea_checksum_ok("$GNGSV,1,1,01,05,45,120,40,1*48"));
}

PROVA(checksum_sbagliato_rifiutato)
{
    VERIFICA(!nmea_checksum_ok(
        "$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*58"));
    // una cifra della latitudine cambiata, checksum originale
    VERIFICA(!nmea_checksum_ok(
        "$GNGGA,123519.00,4807.0390000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57"));
}

PROVA(checksum_esadecimale_minuscolo_accettato)
{
    VERIFICA(nmea_checksum_ok("$GNGSV,1,1,01,05,45,120,40,1*48"));
    VERIFICA(nmea_checksum_ok("$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5d"));
}

PROVA(riga_senza_asterisco_rifiutata)
{
    VERIFICA(!nmea_checksum_ok("$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000"));
}

PROVA(riga_troncata_rifiutata)
{
    VERIFICA(!nmea_checksum_ok("$GNGSV,1,1,01,05,45,120,40,1*4")); // una cifra persa
    VERIFICA(!nmea_checksum_ok("$GNGSV,1,1,01,05,45,120,40,1*"));  // nessuna cifra
    VERIFICA(!nmea_checksum_ok("$GNGSV,1,1,01,05,45"));            // meta' riga
    VERIFICA(!nmea_checksum_ok("$"));
    VERIFICA(!nmea_checksum_ok(""));
    VERIFICA(!nmea_checksum_ok(NULL));
}

PROVA(riga_senza_dollaro_rifiutata)
{
    VERIFICA(!nmea_checksum_ok("GNGSV,1,1,01,05,45,120,40,1*48"));
    VERIFICA(!nmea_checksum_ok("#GNGSV,1,1,01,05,45,120,40,1*48"));
}

PROVA(checksum_con_caratteri_non_esadecimali_rifiutato)
{
    VERIFICA(!nmea_checksum_ok("$GNGSV,1,1,01,05,45,120,40,1*4G"));
    VERIFICA(!nmea_checksum_ok("$GNGSV,1,1,01,05,45,120,40,1*G8"));
}

PROVA(gga_rtk_fisso_letta)
{
    azzera();
    gnss_fix_parse_gga(GGA_FISSO);
    gnss_fix_status_t s = gnss_fix_get_status();
    VERIFICA(s.valid);
    VERIFICA_INT(s.quality, GNSS_FIX_RTK_FIXED);
    VERIFICA_INT(s.satellites_used, 12);
    VERIFICA_VICINO(s.hdop, 0.8, 1e-6);
    VERIFICA_VICINO(s.altitude_m, 545.4, 1e-4);
    VERIFICA_VICINO(s.geoid_sep_m, 46.9, 1e-4);
    VERIFICA_VICINO(s.diff_age_s, 1.0, 1e-6);
    VERIFICA(s.has_position);
    VERIFICA_VICINO(s.lat_deg, 48.0 + 7.038 / 60.0, 1e-10);
    VERIFICA_VICINO(s.lon_deg, 11.0 + 31.0 / 60.0, 1e-10);
    VERIFICA(s.last_update_us > 0);
    VERIFICA_STR(gnss_fix_quality_str(s.quality), "RTK fisso");
}

PROVA(gga_rtk_float_letta)
{
    azzera();
    gnss_fix_parse_gga(GGA_FLOAT);
    gnss_fix_status_t s = gnss_fix_get_status();
    VERIFICA_INT(s.quality, GNSS_FIX_RTK_FLOAT);
    VERIFICA_VICINO(s.diff_age_s, 2.0, 1e-6);
    VERIFICA_STR(gnss_fix_quality_str(s.quality), "RTK float");
}

PROVA(gga_emisferi_sud_ovest_negativi)
{
    azzera();
    gnss_fix_parse_gga(GGA_SUD_OVEST);
    gnss_fix_status_t s = gnss_fix_get_status();
    VERIFICA(s.has_position);
    // 33 52.1234567' S, 070 38.7654321' W: gradi + minuti/60, alta precisione
    VERIFICA_VICINO(s.lat_deg, -(33.0 + 52.1234567 / 60.0), 1e-11);
    VERIFICA_VICINO(s.lon_deg, -(70.0 + 38.7654321 / 60.0), 1e-11);
    VERIFICA_VICINO(s.geoid_sep_m, -25.1, 1e-4);
    VERIFICA_VICINO(s.diff_age_s, -1, 0); // campo vuoto: nessuna correzione
}

PROVA(gga_campi_vuoti_senza_posizione)
{
    azzera();
    gnss_fix_parse_gga(GGA_VUOTA);
    gnss_fix_status_t s = gnss_fix_get_status();
    VERIFICA(s.valid);
    VERIFICA_INT(s.quality, GNSS_FIX_NONE);
    VERIFICA(!s.has_position);
    VERIFICA_VICINO(s.altitude_m, -9999, 0);
    VERIFICA_VICINO(s.geoid_sep_m, -9999, 0);
    VERIFICA_VICINO(s.diff_age_s, -1, 0);
    VERIFICA_VICINO(s.hdop, 99.9, 1e-4);
}

PROVA(gga_corrotta_non_cambia_lo_stato)
{
    azzera();
    gnss_fix_parse_gga(GGA_FISSO);
    gnss_fix_status_t prima = gnss_fix_get_status();
    // Stessa riga con una cifra persa nella latitudine: checksum sbagliato
    gnss_fix_parse_gga("$GNGGA,123519.00,407.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57");
    // Due righe incollate (byte persi sulla seriale)
    gnss_fix_parse_gga("$GNGGA,123519.00,4807.0380000,N,01131.00$GNGGA,123520.00,4807.0380000,N,01131.0000000,E,4,12*57");
    // Senza checksum
    gnss_fix_parse_gga("$GNGGA,123519.00,4907.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000");
    gnss_fix_status_t dopo = gnss_fix_get_status();
    VERIFICA_VICINO(dopo.lat_deg, prima.lat_deg, 0);
    VERIFICA_INT(dopo.last_update_us, prima.last_update_us);
}

PROVA(gga_prima_di_init_ignorata)
{
    // Senza gnss_fix_init() (mutex assente) la riga si ignora, niente crash.
    s_mutex = NULL;
    memset(&s_status, 0, sizeof(s_status));
    gnss_fix_parse_gga(GGA_FISSO);
    gnss_fix_status_t s = gnss_fix_get_status();
    VERIFICA(!s.valid);
}

PROVA(gga_troppo_corta_ignorata)
{
    azzera();
    // checksum giusto ma solo 3 campi: nessun campo qualita'
    char riga[64];
    const char *corpo = "GNGGA,123519.00,4807.038";
    unsigned char cs = 0;
    for (const char *c = corpo; *c; c++) cs ^= (unsigned char) *c;
    snprintf(riga, sizeof(riga), "$%s*%02X", corpo, cs);
    VERIFICA(nmea_checksum_ok(riga));
    gnss_fix_parse_gga(riga);
    VERIFICA(!gnss_fix_get_status().valid);
}

PROVA(qualita_sconosciuta_descritta)
{
    VERIFICA_STR(gnss_fix_quality_str((gnss_fix_quality_t) 42), "Sconosciuto");
    VERIFICA_STR(gnss_fix_quality_str(GNSS_FIX_NONE), "Nessun fix");
}

int main(void)
{
    ESEGUI(checksum_giusto_accettato);
    ESEGUI(checksum_sbagliato_rifiutato);
    ESEGUI(checksum_esadecimale_minuscolo_accettato);
    ESEGUI(riga_senza_asterisco_rifiutata);
    ESEGUI(riga_troncata_rifiutata);
    ESEGUI(riga_senza_dollaro_rifiutata);
    ESEGUI(checksum_con_caratteri_non_esadecimali_rifiutato);
    ESEGUI(gga_rtk_fisso_letta);
    ESEGUI(gga_rtk_float_letta);
    ESEGUI(gga_emisferi_sud_ovest_negativi);
    ESEGUI(gga_campi_vuoti_senza_posizione);
    ESEGUI(gga_corrotta_non_cambia_lo_stato);
    ESEGUI(gga_prima_di_init_ignorata);
    ESEGUI(gga_troppo_corta_ignorata);
    ESEGUI(qualita_sconosciuta_descritta);
    return prove_fine();
}
