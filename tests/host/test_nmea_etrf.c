// Prove di main/nmea_etrf.c: riscrittura delle frasi GGA/RMC da ITRF2020 a
// ETRF2000 (riserva Galileo HAS del rover) e lettura dei campi della GGA.
//
// Valori attesi calcolati in Python con i parametri EUREF (EPSG 10586) e un
// metodo diverso per le geodetiche (Heikkinen): tests/host/strumenti/
// riferimenti.py, sezioni "ETRF", "GGA attesa" ed "ETRF casi limite".
#include "prove.h"
#include "../../main/etrf.c"
#include "../../main/nmea_etrf.c"

#include <stdio.h>
#include <stdlib.h>

// Checksum ricontrollato qui in modo indipendente dal firmware: "$...*HH"
// con due cifre esadecimali maiuscole e niente dopo.
static bool checksum_valido(const char *r)
{
    if (r[0] != '$') return false;
    const char *star = strchr(r, '*');
    if (!star || strlen(star) != 3) return false;
    unsigned cs = 0;
    for (const char *p = r + 1; p < star; p++) cs ^= (unsigned char) *p;
    char atteso[4];
    snprintf(atteso, sizeof(atteso), "%02X", cs);
    return strcmp(star + 1, atteso) == 0;
}

// Campo n (da 0) della riga, senza checksum.
static void campo(const char *r, int n, char *out, size_t cap)
{
    const char *p = r;
    for (int i = 0; i < n && p; i++) {
        p = strchr(p, ',');
        if (p) p++;
    }
    size_t k = 0;
    while (p && *p && *p != ',' && *p != '*' && k + 1 < cap) out[k++] = *p++;
    out[k] = '\0';
}

// "ddmm.mmmm" -> minuti dopo i gradi (gradi_cifre = 2 lat, 3 lon)
static double minuti(const char *f, int gradi_cifre)
{
    return atof(f + gradi_cifre);
}

static const char *GGA_IN = "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,*43";
static const char *RMC_IN = "$GNRMC,101010.00,A,4100.0000000,N,01630.0000000,E,0.01,0.0,091026,,,A,V*07";

PROVA(righe_di_partenza_hanno_checksum_giusto)
{
    VERIFICA(checksum_valido(GGA_IN));
    VERIFICA(checksum_valido(RMC_IN));
}

PROVA(gga_convertita_come_calcolo_python)
{
    // Python: 41 N 16.5 E h 450 (409.5 + 40.5) epoca 2026.5 ->
    // 40 59.9996551' N, 016 29.9994123' E, h 450.0048 -> quota 409.5048
    char r[128];
    strcpy(r, GGA_IN);
    VERIFICA(nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA(checksum_valido(r));
    char f[32];
    campo(r, 2, f, sizeof(f));
    VERIFICA_INT(strlen(f), strlen("4100.0000000")); // stessi decimali dell'originale
    VERIFICA(strncmp(f, "40", 2) == 0);
    VERIFICA_VICINO(minuti(f, 2), 59.9996551, 1.5e-7);
    campo(r, 3, f, sizeof(f)); VERIFICA_STR(f, "N");
    campo(r, 4, f, sizeof(f));
    VERIFICA_INT(strlen(f), strlen("01630.0000000"));
    VERIFICA(strncmp(f, "016", 3) == 0);
    VERIFICA_VICINO(minuti(f, 3), 29.9994123, 1.5e-7);
    campo(r, 5, f, sizeof(f)); VERIFICA_STR(f, "E");
    campo(r, 9, f, sizeof(f));
    VERIFICA_VICINO(atof(f), 409.5048, 0.00011);
    VERIFICA_INT(strlen(f), strlen("409.5000"));
    campo(r, 11, f, sizeof(f)); VERIFICA_STR(f, "40.500"); // separazione invariata
    campo(r, 6, f, sizeof(f)); VERIFICA_STR(f, "1");       // qualita' invariata
    // gli altri campi restano identici
    VERIFICA(strncmp(r, "$GNGGA,101010.00,", 17) == 0);
    VERIFICA(strstr(r, ",1,10,0.9,") != NULL);
}

PROVA(spostamento_in_italia_circa_un_metro_verso_sud_ovest)
{
    // 45.5 N 9.2 E nel 2026.0: Python dN -0.6530 m, dE -0.7224 m (0,974 m)
    double lat = 45.5, lon = 9.2, h = 150.0;
    etrf_itrf2020_to_etrf2000(&lat, &lon, &h, 2026.0);
    double dn = (lat - 45.5) * M_PI / 180.0 * 6367000.0; // raggio meridiano ~ a 45 gradi
    double de = (lon - 9.2) * M_PI / 180.0 * 6389000.0 * cos(45.5 * M_PI / 180.0);
    VERIFICA(dn < 0 && de < 0);
    VERIFICA_VICINO(hypot(dn, de), 0.974, 0.01);
    VERIFICA_VICINO(lat, 45.4999941244, 1e-9);  // 0,1 mm circa
    VERIFICA_VICINO(lon, 9.1999907574, 1e-9);
    VERIFICA_VICINO(h, 149.9920, 0.0005);
}

PROVA(rmc_convertita_con_quota_data)
{
    char r[128];
    strcpy(r, RMC_IN);
    VERIFICA(nmea_etrf_convert(r, sizeof(r), 2026.5, 450.0));
    VERIFICA(checksum_valido(r));
    char f[32];
    campo(r, 3, f, sizeof(f));
    VERIFICA_VICINO(minuti(f, 2), 59.9996551, 1.5e-7);
    campo(r, 4, f, sizeof(f)); VERIFICA_STR(f, "N");
    campo(r, 5, f, sizeof(f));
    VERIFICA_VICINO(minuti(f, 3), 29.9994123, 1.5e-7);
    campo(r, 6, f, sizeof(f)); VERIFICA_STR(f, "E");
    campo(r, 2, f, sizeof(f)); VERIFICA_STR(f, "A");
    VERIFICA(strstr(r, ",0.01,0.0,091026,,,A,V*") != NULL);
}

PROVA(riporto_dei_minuti_a_60_sui_gradi)
{
    // Con 2 decimali 40 59.99965' si arrotonda a 60.00: deve diventare 41 00.00
    char r[128];
    const char *in = "$GNGGA,101010.00,4100.00,N,01630.00,E,1,10,0.9,409.5,M,40.5,M,,";
    unsigned cs = 0;
    for (const char *p = in + 1; *p; p++) cs ^= (unsigned char) *p;
    snprintf(r, sizeof(r), "%s*%02X", in, cs);
    VERIFICA(nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA(checksum_valido(r));
    char f[32];
    campo(r, 2, f, sizeof(f)); VERIFICA_STR(f, "4100.00");
    campo(r, 4, f, sizeof(f)); VERIFICA_STR(f, "01630.00");
    // e direttamente sulla formattazione
    format_coord(f, sizeof(f), 40.9999999999, 2, 4);
    VERIFICA_STR(f, "4100.0000");
    format_coord(f, sizeof(f), -16.99999999999, 3, 7);
    VERIFICA_STR(f, "01700.0000000");
    format_coord(f, sizeof(f), 9.5, 3, 0);
    VERIFICA_STR(f, "00930");
}

PROVA(emisferi_sud_ovest_conservati)
{
    // Python: 33 52.1234567' S 070 38.7654321' W h -12.8 -> -33.868727687400
    // -70.646101063308 h -12.7455 -> 33 52.1236612' S, 070 38.7660638' W,
    // quota 12.3545 -> "12.4" (un decimale come l'originale)
    char r[128];
    strcpy(r, "$GPGGA,000001.00,3352.1234567,S,07038.7654321,W,1,08,1.2,12.3,M,-25.1,M,,*79");
    VERIFICA(nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA(checksum_valido(r));
    char f[32];
    campo(r, 2, f, sizeof(f)); VERIFICA(strncmp(f, "33", 2) == 0);
    VERIFICA_VICINO(minuti(f, 2), 52.1236612, 1.5e-7);
    campo(r, 3, f, sizeof(f)); VERIFICA_STR(f, "S");
    campo(r, 4, f, sizeof(f)); VERIFICA(strncmp(f, "070", 3) == 0);
    VERIFICA_VICINO(minuti(f, 3), 38.7660638, 1.5e-7);
    campo(r, 5, f, sizeof(f)); VERIFICA_STR(f, "W");
    campo(r, 9, f, sizeof(f)); VERIFICA_STR(f, "12.4");
}

PROVA(emisfero_cambiato_vicino_a_equatore_e_greenwich)
{
    // Python: 0.00001' N 0.00001' E -> -0.000005892095, -0.000007618006 gradi
    // = 0.0003535' S, 0.0004571' W
    char r[128];
    const char *in = "$GNGGA,101010.00,0000.0000100,N,00000.0000100,E,1,10,0.9,0.000,M,0.000,M,,";
    unsigned cs = 0;
    for (const char *p = in + 1; *p; p++) cs ^= (unsigned char) *p;
    snprintf(r, sizeof(r), "%s*%02X", in, cs);
    VERIFICA(nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA(checksum_valido(r));
    char f[32];
    campo(r, 2, f, sizeof(f)); VERIFICA_STR(f, "0000.0003535");
    campo(r, 3, f, sizeof(f)); VERIFICA_STR(f, "S");
    campo(r, 4, f, sizeof(f)); VERIFICA_STR(f, "00000.0004571");
    campo(r, 5, f, sizeof(f)); VERIFICA_STR(f, "W");
}

PROVA(righe_non_gga_rmc_o_senza_posizione_invariate)
{
    char r[128];
    const char *gsv = "$GNGSV,1,1,01,05,45,120,40,1*48";
    strcpy(r, gsv);
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA_STR(r, gsv);
    const char *vuota = "$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5D";
    strcpy(r, vuota);
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA_STR(r, vuota);
    strcpy(r, "GNGGA senza dollaro");
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    strcpy(r, "$GN");
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
}

PROVA(buffer_troppo_piccolo_riga_invariata)
{
    char r[128];
    strcpy(r, GGA_IN);
    size_t n = strlen(GGA_IN);
    // la riga convertita ha la stessa lunghezza: serve n + 1 byte
    VERIFICA(!nmea_etrf_convert(r, n, 2026.5, 0));
    VERIFICA_STR(r, GGA_IN);
    VERIFICA(nmea_etrf_convert(r, n + 1, 2026.5, 0));
    VERIFICA_INT(strlen(r), n);
}

PROVA(riga_troppo_lunga_rifiutata)
{
    char r[260];
    memset(r, 0, sizeof(r));
    strcpy(r, "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5,M,40.5,M,,");
    while (strlen(r) < 210) strcat(r, "0");
    strcat(r, "*00");
    char copia[260];
    strcpy(copia, r);
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA_STR(r, copia);
}

PROVA(gga_qualita_quota_e_latlon_lette)
{
    VERIFICA_INT(nmea_gga_quality(GGA_IN), 1);
    VERIFICA_INT(nmea_gga_quality("$GNGGA,123519.00,4807.0380000,N,01131.0000000,E,4,12,0.8,545.4,M,46.9,M,1.0,0000*57"), 4);
    VERIFICA_INT(nmea_gga_quality(RMC_IN), -1);
    VERIFICA_INT(nmea_gga_quality("$GPGGA,000002.00,,,,,,00,99.9,,,,,,*5D"), -1); // qualita' vuota
    double h = 0;
    VERIFICA(nmea_gga_ellipsoidal_height(GGA_IN, &h));
    VERIFICA_VICINO(h, 450.0, 1e-9);
    VERIFICA(!nmea_gga_ellipsoidal_height("$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5D", &h));
    double lat = 0, lon = 0;
    VERIFICA(nmea_gga_latlon("$GPGGA,000001.00,3352.1234567,S,07038.7654321,W,1,08,1.2,12.3,M,-25.1,M,,*79", &lat, &lon));
    VERIFICA_VICINO(lat, -(33 + 52.1234567 / 60.0), 1e-11);
    VERIFICA_VICINO(lon, -(70 + 38.7654321 / 60.0), 1e-11);
    VERIFICA(!nmea_gga_latlon("$GPGGA,000002.00,,,,,0,00,99.9,,,,,,*5D", &lat, &lon));
    VERIFICA(!nmea_gga_latlon(RMC_IN, &lat, &lon));
}

PROVA(posizione_has_messa_nella_gga)
{
    // Posizione ITRF2020 data (soluzione HAS): 41 00.5' N, 16.5 E, h 450 ->
    // Python: 41.008327584383, 16.499990203992, 450.0048 -> quota 409.5048
    char r[128];
    strcpy(r, "$GNGGA,101010.00,4100.00,N,01630.00,E,1,10,0.9,409.5,M,40.500,M,1.0,0000");
    unsigned cs = 0;
    for (const char *p = r + 1; *p; p++) cs ^= (unsigned char) *p;
    snprintf(r + strlen(r), 8, "*%02X", cs);
    VERIFICA(nmea_etrf_set_position(r, sizeof(r), 2026.5, 41.0 + 0.5 / 60, 16.5, 450.0, 5, 3.0f));
    VERIFICA(checksum_valido(r));
    char f[32];
    campo(r, 2, f, sizeof(f));
    VERIFICA_INT(strlen(f), strlen("4100.0000000")); // almeno 7 decimali per HAS
    VERIFICA_VICINO(minuti(f, 2), (41.008327584383 - 41) * 60, 1.5e-7);
    campo(r, 4, f, sizeof(f));
    VERIFICA_VICINO(minuti(f, 3), (16.499990203992 - 16) * 60, 1.5e-7);
    campo(r, 6, f, sizeof(f)); VERIFICA_STR(f, "5");
    campo(r, 9, f, sizeof(f)); VERIFICA_STR(f, "409.505"); // 3 decimali
    campo(r, 13, f, sizeof(f)); VERIFICA_STR(f, "3.0");     // eta' delle correzioni, un decimale
    campo(r, 14, f, sizeof(f)); VERIFICA_STR(f, "0000");
}

PROVA(anno_decimale_della_data)
{
    // 1 gennaio 2026 00:00 UTC = 1767225600; 1 luglio 2026 12:00 = 1782907200
    VERIFICA_VICINO(etrf_decimal_year((time_t) 1767225600), 2026.0, 1e-9);
    VERIFICA_VICINO(etrf_decimal_year((time_t) 1782907200), 2026.0 + (181.5 / 365.0), 1e-9);
    // anno bisestile: 1 marzo 2028 00:00 = giorno 60
    VERIFICA_VICINO(etrf_decimal_year((time_t) 1835481600), 2028.0 + 60.0 / 366.0, 1e-9);
}

// "$corpo*HH" con il checksum giusto.
static void con_checksum(char *riga, size_t cap, const char *corpo)
{
    unsigned cs = 0;
    for (const char *c = corpo; *c; c++) cs ^= (unsigned char) *c;
    snprintf(riga, cap, "$%s*%02X", corpo, cs);
}

// Trovato dal fuzzing (bersagli nmea_etrf e gnss_nmea_reader): coordinata
// fuori scala in una GGA/RMC (checksum giusto). parse_coord (nmea_etrf.c)
// converte x/100 in int (comportamento indefinito con "1e300") e poi
// moltiplica d * 100 in int (overflow con "4000000000.0"): la riga
// riscritta per AgOpenGPS e il caster avrebbe coordinate a caso.
// Correzione proposta: in parse_coord rifiutare (false) valori non finiti o
// fuori scala (lat >= 9000, lon >= 18000), prima delle conversioni in int.
PROVA(coordinate_fuori_scala_rifiutate)
{
    char r[200], copia[200];
    double lat, lon;
    con_checksum(r, sizeof(r), "GNGGA,101010.00,1e300,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,");
    VERIFICA(!nmea_gga_latlon(r, &lat, &lon));
    strcpy(copia, r);
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA_STR(r, copia);
    con_checksum(r, sizeof(r), "GNRMC,101010.00,A,4100.0000000,N,4000000000.0,E,0.01,0.0,091026,,,A,V");
    strcpy(copia, r);
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 450.0));
    VERIFICA_STR(r, copia);
}

// Trovato dal fuzzing (bersaglio nmea_etrf): posizione data non numerica o
// enorme (soluzione HAS da #PPPNAVA: atof di un campo rovinato, o "nan").
// format_coord (nmea_etrf.c) converte i gradi in int: comportamento
// indefinito, e nella GGA per AgOpenGPS finirebbe "nan" o un numero a caso.
// In gnss_nmea_reader.c il controllo del salto (hypot(...) > 10 m) non ferma
// un NaN (il confronto e' falso). Correzione proposta: in
// nmea_etrf_set_position (o in rewrite) rifiutare lat/lon/quota non finite o
// fuori da +-90 / +-180; in has_fallback_line scrivere il controllo come
// !(hypot(dn, de) <= HAS_PPP_MAX_JUMP_M).
PROVA(posizione_has_non_numerica_rifiutata)
{
    char r[128];
    strcpy(r, GGA_IN);
    VERIFICA(!nmea_etrf_set_position(r, sizeof(r), 2026.5, NAN, 16.5, 450.0, 2, 3.0f));
    VERIFICA_STR(r, GGA_IN);
    VERIFICA(!nmea_etrf_set_position(r, sizeof(r), 2026.5, 41.0, 1e300, 450.0, 2, 3.0f));
    VERIFICA_STR(r, GGA_IN);
}

// Trovato dal fuzzing (bersaglio nmea_etrf): una riga lunga (vicina ai 199
// caratteri ammessi) che la riscrittura allunga (piu' decimali per la
// posizione HAS, quota, eta' delle correzioni) supera i 216 byte del buffer
// statico "out" di rewrite: l'ultimo snprintf del checksum viene troncato
// ("*4" invece di "*4B") ma il controllo confronta con cap invece che con
// sizeof(out), quindi la riga esce con il checksum rotto e il memcpy finale
// legge 1-3 byte oltre "out". Correzione proposta in rewrite (nmea_etrf.c):
// if (w < 0 || (size_t) w >= sizeof(out) - k || k + (size_t) w >= cap) return false;
PROVA(riga_lunga_allungata_checksum_intero)
{
    char corpo[260];
    strcpy(corpo, "GNGGA,101010.00,4100.0,N,01630.0,E,1,10,0.9,409.5,M,40.500,M,,");
    while (strlen(corpo) < 199 - 4) strcat(corpo, "0"); // id stazione lungo: riga di 199 caratteri
    char r[320];
    con_checksum(r, sizeof(r), corpo);
    VERIFICA_INT(strlen(r), 199);
    bool ok = nmea_etrf_set_position(r, sizeof(r), 2026.5, 41.0, 16.5, 450.0, 2, 3.0f);
    VERIFICA(!ok || checksum_valido(r));
}

// Trovato dal fuzzing il 09/10/2026: quota o separazione del geoide enormi
// ("1e1200" = infinito) davano lat/lon non numeriche dopo la trasformazione
// e poi una conversione in int non definita in format_coord.
PROVA(quota_infinita_rifiutata)
{
    char r[200], copia[200];
    con_checksum(r, sizeof(r), "GNGGA,101010.00,4107.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.5e1200,M,,");
    strcpy(copia, r);
    VERIFICA(!nmea_etrf_convert(r, sizeof(r), 2026.5, 0));
    VERIFICA_STR(r, copia);
    strcpy(r, GGA_IN);
    VERIFICA(!nmea_etrf_set_position(r, sizeof(r), 2026.5, 41.0, 16.5, INFINITY, 2, 3.0f));
}

int main(void)
{
    ESEGUI(righe_di_partenza_hanno_checksum_giusto);
    ESEGUI(gga_convertita_come_calcolo_python);
    ESEGUI(spostamento_in_italia_circa_un_metro_verso_sud_ovest);
    ESEGUI(rmc_convertita_con_quota_data);
    ESEGUI(riporto_dei_minuti_a_60_sui_gradi);
    ESEGUI(emisferi_sud_ovest_conservati);
    ESEGUI(emisfero_cambiato_vicino_a_equatore_e_greenwich);
    ESEGUI(righe_non_gga_rmc_o_senza_posizione_invariate);
    ESEGUI(buffer_troppo_piccolo_riga_invariata);
    ESEGUI(riga_troppo_lunga_rifiutata);
    ESEGUI(gga_qualita_quota_e_latlon_lette);
    ESEGUI(posizione_has_messa_nella_gga);
    ESEGUI(anno_decimale_della_data);
    ESEGUI(coordinate_fuori_scala_rifiutate);
    ESEGUI(posizione_has_non_numerica_rifiutata);
    ESEGUI(riga_lunga_allungata_checksum_intero);
    ESEGUI(quota_infinita_rifiutata);
    return prove_fine();
}
