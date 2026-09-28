#include "gnss_signal.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static gnss_sat_signal_t s_sats[GNSS_SIGNAL_MAX_SATS];
static SemaphoreHandle_t s_mutex;

// Ultimo svuotamento per costellazione (talker "GP", "GL", ...). Un
// ricevitore multibanda (es. ZED-F9P, NMEA 4.11) manda a ogni epoca un
// ciclo di GSV per ciascun segnale (L1, poi L2, ...), ognuno che riparte da
// "messaggio 1": svuotare a ogni "messaggio 1" lasciava solo l'ultimo
// segnale, tipicamente L2, spesso a 0 dB-Hz al chiuso - grafico con tutti i
// satelliti a zero anche con il fix valido (visto sul dispositivo). Ora si
// svuota una volta per epoca e si tiene il segnale migliore tra le bande.
#define GSV_EPOCH_GAP_US 300000
#define GSV_MAX_TALKERS  8
static struct {
    char talker[2];
    int64_t last_clear_us;
} s_clears[GSV_MAX_TALKERS];

static bool should_clear_constellation(const char *talker)
{
    int64_t now = esp_timer_get_time();
    int free_slot = -1;
    for (int i = 0; i < GSV_MAX_TALKERS; i++) {
        if (s_clears[i].talker[0] == talker[0] && s_clears[i].talker[1] == talker[1]) {
            if (now - s_clears[i].last_clear_us < GSV_EPOCH_GAP_US) {
                return false; // stessa epoca, altro segnale: si aggiunge
            }
            s_clears[i].last_clear_us = now;
            return true;
        }
        if (free_slot < 0 && s_clears[i].talker[0] == '\0') {
            free_slot = i;
        }
    }
    if (free_slot >= 0) {
        s_clears[free_slot].talker[0] = talker[0];
        s_clears[free_slot].talker[1] = talker[1];
        s_clears[free_slot].last_clear_us = now;
    }
    return true;
}

void gnss_signal_init(void)
{
    memset(s_sats, 0, sizeof(s_sats));
    s_mutex = xSemaphoreCreateMutex();
}

// Isola il prossimo campo separato da virgola in *cursor, terminandolo
// con '\0' (o troncando su '*' per l'ultimo campo prima del checksum).
// Muta la stringa originale - va lavorato su una copia locale.
static char *next_field(char **cursor)
{
    if (!*cursor) {
        return NULL;
    }
    char *start = *cursor;
    char *comma = strchr(start, ',');
    if (comma) {
        *comma = '\0';
        *cursor = comma + 1;
    } else {
        char *star = strchr(start, '*');
        if (star) {
            *star = '\0';
        }
        *cursor = NULL;
    }
    return start;
}

void gnss_signal_parse_gsv(const char *line_in)
{
    if (!s_mutex || !line_in || line_in[0] != '$' || strlen(line_in) < 6) {
        return;
    }

    char line[128];
    strncpy(line, line_in, sizeof(line) - 1);
    line[sizeof(line) - 1] = '\0';

    char constellation[3] = { line[1], line[2], '\0' };

    char *cursor = line;
    next_field(&cursor); // "$xxGSV"
    next_field(&cursor); // numero totale di sentenze per questo ciclo
    char *msg_num = next_field(&cursor);
    next_field(&cursor); // satelliti totali in vista (non usato)

    bool is_first_message = msg_num && strcmp(msg_num, "1") == 0;

    xSemaphoreTake(s_mutex, portMAX_DELAY);

    if (is_first_message && should_clear_constellation(constellation)) {
        // Ripulisce le vecchie voci di questa costellazione a inizio
        // epoca (non a ogni segnale, vedi s_clears), cosi' un satellite
        // non piu' in vista sparisce dal grafico invece di restare
        // "congelato" con l'ultimo SNR noto.
        for (int i = 0; i < GNSS_SIGNAL_MAX_SATS; i++) {
            if (s_sats[i].used && memcmp(s_sats[i].constellation, constellation, 2) == 0) {
                s_sats[i].used = false;
            }
        }
    }

    while (1) {
        char *prn = next_field(&cursor);
        char *elev = next_field(&cursor);
        char *azim = next_field(&cursor);
        char *snr = next_field(&cursor);
        (void) elev;
        (void) azim;
        if (!prn || prn[0] == '\0' || !elev) {
            // !elev: gruppo incompleto, es. il campo "Signal ID" che
            // u-blox (NMEA 4.11) aggiunge in fondo alla sentenza anche
            // con zero satelliti ("$GPGSV,1,1,00,0*65") - senza questo
            // controllo veniva scambiato per il PRN di un satellite
            // inesistente.
            break;
        }

        int prn_val = atoi(prn);
        int snr_val = (snr && snr[0] != '\0') ? atoi(snr) : 0;

        int slot = -1;
        for (int i = 0; i < GNSS_SIGNAL_MAX_SATS; i++) {
            if (s_sats[i].used && s_sats[i].prn == (uint16_t) prn_val &&
                memcmp(s_sats[i].constellation, constellation, 2) == 0) {
                slot = i;
                break;
            }
        }
        if (slot < 0) {
            for (int i = 0; i < GNSS_SIGNAL_MAX_SATS; i++) {
                if (!s_sats[i].used) {
                    slot = i;
                    break;
                }
            }
        }
        if (slot >= 0) {
            // Stesso satellite gia' visto in questa epoca su un'altra banda:
            // si tiene il segnale migliore.
            bool already = s_sats[slot].used;
            if (!already || snr_val > s_sats[slot].snr) {
                s_sats[slot].snr = (uint8_t) snr_val;
            }
            s_sats[slot].used = true;
            s_sats[slot].prn = (uint16_t) prn_val;
            memcpy(s_sats[slot].constellation, constellation, 3);
        }

        if (!snr) {
            break; // fine campi disponibili in questa sentenza (ultimo gruppo incompleto)
        }
    }

    xSemaphoreGive(s_mutex);
}

size_t gnss_signal_get_satellites(gnss_sat_signal_t *out, size_t max_sats)
{
    if (!s_mutex) {
        return 0;
    }
    size_t count = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < GNSS_SIGNAL_MAX_SATS && count < max_sats; i++) {
        if (s_sats[i].used) {
            out[count++] = s_sats[i];
        }
    }
    xSemaphoreGive(s_mutex);
    return count;
}
