#include "gnss_signal.h"

#include <string.h>
#include <stdlib.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static gnss_sat_signal_t s_sats[GNSS_SIGNAL_MAX_SATS];
static SemaphoreHandle_t s_mutex;

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

    if (is_first_message) {
        // Ripulisce le vecchie voci di questa costellazione a inizio
        // ciclo, cosi' un satellite non piu' in vista sparisce dal
        // grafico invece di restare "congelato" con l'ultimo SNR noto.
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
        if (!prn || prn[0] == '\0') {
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
            s_sats[slot].used = true;
            s_sats[slot].prn = (uint16_t) prn_val;
            s_sats[slot].snr = (uint8_t) snr_val;
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
