#include "gnss_nmea_reader.h"
#include "license.h"
#include "sys_stats.h"
#include "time_sync.h"
#include "gnss_io.h"
#include "nmea_udp_broadcast.h"
#include "gnss_signal.h"
#include "gnss_fix.h"
#include "gnss_ubx_ack.h"
#include "gnss_unicore.h"
#include "gnss_comnav.h"
#include "nmea_etrf.h"
#include "etrf.h"
#include "settings.h"
#include "esp_timer.h"
#include <time.h>
#include <math.h>
#include "ntrip_rover_client.h"
#include "status.h"

#include <string.h>
#include <stdint.h>
#include <stdlib.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define READ_BUF_SIZE 256
// 128 bastava per le sole righe NMEA; le righe ASCII Bynav #INSPVAXA (lat/
// lon/quota/velocita'/assetto/deviazioni standard, ~20 campi) sono molto
// piu' lunghe - alzato per non troncarle (vedi handle_bynav_line() sotto).
#define LINE_BUF_SIZE 320

// Estrae il campo N (0-based) della sezione dati di una riga ASCII in stile
// Bynav/NovAtel OEM7 (nomi di log come #INSPVAXA/#HEADINGA riusati
// identici da Bynav): intestazione e dati separati dal primo ';', campi
// dati separati da ',', fino all'eventuale checksum "*xx" finale. Ritorna
// false se il campo richiesto non esiste (riga troncata/malformata) -
// meglio scartare il valore che usarne uno spostato di campo.
static bool bynav_data_field(const char *line, int field_index, char *out, size_t out_size)
{
    const char *data = strchr(line, ';');
    if (!data) {
        return false;
    }
    data++;
    for (int i = 0; i < field_index; i++) {
        data = strchr(data, ',');
        if (!data) {
            return false;
        }
        data++;
    }
    const char *end = data;
    while (*end && *end != ',' && *end != '*') {
        end++;
    }
    size_t len = (size_t)(end - data);
    if (len >= out_size) {
        len = out_size - 1;
    }
    memcpy(out, data, len);
    out[len] = '\0';
    return true;
}

// Layout campi dati basato sul formato NovAtel OEM7 (Bynav riusa gli stessi
// nomi di log #INSPVAXA/#HEADINGA e la stessa convenzione intestazione/dati
// - vedi AN065_QuickStart.pdf) - MAI verificato contro l'output reale di un
// modulo Bynav M21D (nessun hardware disponibile in questo progetto). Da
// controllare via log seriale reale prima di fare affidamento su questi
// valori per qualunque uso in campo (es. autosterzo).
//
// #INSPVAXA campi dati: 0=ins_status, 1=pos_type, 2=lat, 3=lon, 4=height,
// 5=undulation, 6=north_vel, 7=east_vel, 8=up_vel, 9=roll, 10=pitch,
// 11=azimuth(heading), poi le deviazioni standard corrispondenti (non lette qui).
static void handle_inspvaxa(const char *line)
{
    char status[24], roll[16], pitch[16], azimuth[16];
    if (!bynav_data_field(line, 0, status, sizeof(status))) return;
    if (!bynav_data_field(line, 9, roll, sizeof(roll))) return;
    if (!bynav_data_field(line, 10, pitch, sizeof(pitch))) return;
    if (!bynav_data_field(line, 11, azimuth, sizeof(azimuth))) return;
    status_bynav_ins_note_inspvaxa(status, (float) atof(roll), (float) atof(pitch), (float) atof(azimuth));
}

// #HEADINGA campi dati: 0=sol_status, 1=pos_type, 2=length, 3=heading,
// 4=pitch, poi deviazioni standard/altri campi (non letti qui). Nessuno
// stato di allineamento INS in questo log (solo in INSPVAXA sopra).
static void handle_headinga(const char *line)
{
    char heading[16], pitch[16];
    if (!bynav_data_field(line, 3, heading, sizeof(heading))) return;
    if (!bynav_data_field(line, 4, pitch, sizeof(pitch))) return;
    status_bynav_ins_note_headinga((float) atof(heading), (float) atof(pitch));
}

// Righe ASCII Bynav (prefisso '#', non NMEA) - solo i due log di prua/
// assetto INS ci interessano qui (GNSS_CHIP_BYNAV_M21D rover, vedi
// gnss_bynav_m21d_configure_rover()); BESTPOSA/INSCALSTATUSA arrivano sulla
// stessa UART ma non sono ancora parsati (vedi nota in status.h).
static void handle_bynav_line(const char *line)
{
    if (strncmp(line, "#INSPVAXA", 9) == 0) {
        handle_inspvaxa(line);
    } else if (strncmp(line, "#HEADINGA", 9) == 0) {
        handle_headinga(line);
    }
}

// "$GxRMC,hhmmss.ss,A,...,ddmmyy,...": ora UTC con data, solo se valida ("A").
static void nmea_rmc_time(const char *line)
{
    const char *f[10] = {0};
    int n = 0;
    for (const char *p = line; *p && n < 10; p++) {
        if (*p == ',') {
            f[n++] = p + 1;
        }
    }
    // f[0] ora, f[1] stato, f[8] data
    if (n < 9 || f[1][0] != 'A' || f[0][0] == ',' || f[8][0] == ',') {
        return;
    }
    int hh = (f[0][0] - '0') * 10 + (f[0][1] - '0');
    int mi = (f[0][2] - '0') * 10 + (f[0][3] - '0');
    int ss = (f[0][4] - '0') * 10 + (f[0][5] - '0');
    int dd = (f[8][0] - '0') * 10 + (f[8][1] - '0');
    int mo = (f[8][2] - '0') * 10 + (f[8][3] - '0');
    int yy = (f[8][4] - '0') * 10 + (f[8][5] - '0');
    time_sync_from_gnss(2000 + yy, mo, dd, hh, mi, ss);
}

// Riserva Galileo HAS (settings.rover_has_fallback, letta all'avvio come
// la configurazione del ricevitore). Le correzioni RTK valgono se l'ultimo
// RTCM dal caster e' arrivato da meno di questo tempo.
#define RTK_FRESH_US (60LL * 1000000)

static bool s_has_fallback;
static bool s_convert;        // ultima GGA fuori dall'RTK: converti anche RMC
static double s_h_ell_m;      // ultima quota ellissoidica, per RMC
static const char *s_has_state = "";

static void peek_has(const app_settings_t *s, void *ctx)
{
    *(bool *) ctx = s->rover_has_fallback && license_has(LIC_HAS);
}

const char *gnss_nmea_reader_has_state(void)
{
    return s_has_state;
}

// In RTK le coordinate sono nel sistema della base (ETRF2000); fuori
// dall'RTK (HAS o autonome) sono ITRF: riscritte in ETRF2000 prima di
// andare ad AgOpenGPS, al caster e al pannello. Cosi' passando da RTK a HAS
// e ritorno la posizione non salta di ~1 m.
// Soluzione HAS (PPPNAVA dell'UM98x) usabile per il rover: recente e con
// incertezza e correzioni nei limiti (stessi della misura della base).
#define HAS_PPP_FRESH_US     (3LL * 1000000)
#define HAS_PPP_MAX_SIGMA_M  1.5f
#define HAS_PPP_MAX_AGE_S    30.0f
#define HAS_PPP_MAX_JUMP_M   10.0 // HAS lontana piu' di cosi' dalla posizione autonoma = salto
                                 // (07/10: normale 1-3 m, salto 17 m con incertezza dichiarata 1,4 m)
#define GGA_QUALITY_HAS      2    // "corretta (DGPS)": non e' RTK, ma non e' autonoma

static gnss_unicore_ppp_t s_ppp;  // ultima soluzione HAS usabile, valida per la riga RMC che segue la GGA
static bool s_use_ppp;

static void has_fallback_line(char *line, size_t *len)
{
    bool gga = memcmp(&line[3], "GGA", 3) == 0;
    if (!gga && memcmp(&line[3], "RMC", 3) != 0) {
        return;
    }
    if (gga) {
        int q = nmea_gga_quality(line);
        int64_t now_us = esp_timer_get_time();
        int64_t last = status_get_last_rtcm_time_us();
        bool fresh = last > 0 && now_us - last < RTK_FRESH_US;
        // 6 = soluzione inerziale (INS) che prosegue l'RTK: stesso sistema della base.
        bool rtk = (q == 4 || q == 5 || q == 6) && fresh;
        s_convert = q >= 1 && !rtk;
        // Con l'UM982 la soluzione HAS non e' nella GGA (resta autonoma,
        // prova del 07/10/2026): si prende dal log PPPNAVA.
        gnss_unicore_ppp_get(&s_ppp);
        s_use_ppp = s_convert && s_ppp.valid && now_us - s_ppp.at_us < HAS_PPP_FRESH_US &&
                    s_ppp.corr_age_s <= HAS_PPP_MAX_AGE_S &&
                    hypotf(s_ppp.sig_lat_m, s_ppp.sig_lon_m) <= HAS_PPP_MAX_SIGMA_M;
        double h;
        if (nmea_gga_ellipsoidal_height(line, &h)) {
            s_h_ell_m = h;
        }
        if (s_use_ppp) {
            double alat, alon; // posizione autonoma di questa stessa riga GGA, prima di riscriverla
            if (nmea_gga_latlon(line, &alat, &alon)) {
                double dn = (s_ppp.lat_deg - alat) * 111320.0;
                double de = (s_ppp.lon_deg - alon) * 111320.0 * cos(alat * M_PI / 180.0);
                if (hypot(dn, de) > HAS_PPP_MAX_JUMP_M) {
                    s_use_ppp = false;
                }
            }
        }
        s_has_state = q < 1 ? "" : (rtk ? "rtk" : (s_use_ppp ? "has" : "autonoma"));
    }
    time_t now = time(NULL);
    if (!s_convert || now < 1700000000) { // senza ora valida niente epoca: riga com'e'
        return;
    }
    double epoch = etrf_decimal_year(now);
    bool ok = s_use_ppp
        ? nmea_etrf_set_position(line, LINE_BUF_SIZE, epoch, s_ppp.lat_deg, s_ppp.lon_deg, s_ppp.h_ell_m,
                                 GGA_QUALITY_HAS, s_ppp.corr_age_s)
        : nmea_etrf_convert(line, LINE_BUF_SIZE, epoch, s_h_ell_m);
    if (ok) {
        *len = strlen(line);
    }
}

void gnss_nmea_reader_task(void *arg)
{
    (void) arg; // porta verso il ricevitore gestita da gnss_io (seriale o I2C)
    uint8_t read_buf[READ_BUF_SIZE];
    char line[LINE_BUF_SIZE];
    size_t line_len = 0;
    settings_peek(peek_has, &s_has_fallback);

    while (1) {
        sys_stats_heartbeat(HB_GNSS);
        int n = gnss_io_read(read_buf, sizeof(read_buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char) read_buf[i];

            // Frame binari UBX-ACK-ACK/NAK (risposta ai comandi di
            // configurazione mandati da gnss_ubx.c) possono arrivare
            // interposti alle righe NMEA sulla stessa UART - la firma
            // 0xB5 0x62 e' inequivocabile (mai presente in NMEA, puro
            // ASCII stampabile), quindi si possono distinguere senza
            // ambiguita' prima di provare a trattarli come NMEA.
            if (gnss_ubx_ack_feed_byte((uint8_t) c)) {
                continue;
            }

            // Una riga comincia sempre da '$' (NMEA) o '#' (log ASCII
            // Unicore/ComNav/Bynav): qui si riparte da capo. Un byte non
            // stampabile (frame binario, es. RXM-RAWX rimasto acceso dalla
            // base) scarta quanto raccolto. Prima i byte binari restavano in
            // testa alla riga e la frase NMEA successiva veniva persa.
            if (c == '$' || c == '#') {
                line_len = 0;
            } else if (c != '\n' && c != '\r' && ((uint8_t) c < 0x20 || (uint8_t) c > 0x7E)) {
                line_len = 0;
                continue;
            }

            if (c == '\n') {
                line[line_len] = '\0';
                // Checksum NMEA: righe incollate o con byte persi (seriale in
                // overflow) non vanno ne' ad AgOpenGPS ne' al caster ne' alla
                // misura della posizione base.
                if (line_len > 6 && line[0] == '$' && nmea_checksum_ok(line)) {
                    if (s_has_fallback) {
                        has_fallback_line(line, &line_len);
                    }

                    nmea_udp_broadcast_send(line, line_len);

                    // Formato NMEA: '$' + talker (2 char) + tipo sentenza
                    // (3 char), es. "$GPGGA"/"$GNGGA"/"$GPGSV"...
                    if (memcmp(&line[3], "RMC", 3) == 0 && time_sync_wants_gnss()) {
                        nmea_rmc_time(line); // ora dai satelliti finche' manca l'NTP
                    }
                    if (memcmp(&line[3], "GGA", 3) == 0) {
                        ntrip_rover_client_forward_gga(line, line_len);
                        gnss_fix_parse_gga(line);
                    } else if (memcmp(&line[3], "GSV", 3) == 0) {
                        gnss_signal_parse_gsv(line);
                    }
                } else if (line_len > 10 && line[0] == '#') {
                    handle_bynav_line(line);
                    gnss_unicore_note_line(line);
                    gnss_comnav_note_line(line);
                }
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < LINE_BUF_SIZE - 1) {
                    line[line_len++] = (char) c;
                } else {
                    // Riga troppo lunga (improbabile per NMEA standard):
                    // scarta e riparte dalla prossima, per non restare
                    // bloccati a concatenare spazzatura all'infinito.
                    line_len = 0;
                }
            }
        }
    }
}
