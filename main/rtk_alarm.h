#pragma once

#include <stdint.h>
#include <stdbool.h>

// Allarme "RTK perso" del rover (1.32.0): il trattore era in RTK fisso
// (qualita' GGA 4) e da piu' di N secondi non lo e' piu' (float, DGPS,
// autonoma, niente fix o nessun dato dal ricevitore). Avvisa sul display e
// nel pannello, cosi' chi guida sa che l'autosterzo non ha piu' la
// precisione di pochi centimetri. Non scatta mai se dall'avvio il rover
// non e' mai stato in RTK fisso (es. base spenta di proposito).
//
// Solo calcolo, senza ESP-IDF ne' FreeRTOS (prove sul PC in
// tests/host/test_rtk_alarm.c): chi legge la GGA (gnss_nmea_reader.c)
// chiama rtk_alarm_step() a ogni giro e tiene la copia per gli altri task.

// Qualita' "nessun dato": nessuna GGA dal ricevitore da RTK_ALARM_NO_DATA_US.
#define RTK_ALARM_NO_DATA     (-1)
#define RTK_ALARM_NO_DATA_US  (2LL * 1000000)
#define RTK_ALARM_MAX_S       600

typedef struct {
    bool had_fixed;        // e' gia' stato in RTK fisso dall'avvio
    bool lost;             // fuori dall'RTK fisso dopo esserci stato
    int64_t lost_since_us; // prima lettura senza RTK fisso (valido se lost)
    int last_quality;      // ultima qualita' (0-8 della GGA o RTK_ALARM_NO_DATA)
} rtk_alarm_t;

typedef struct {
    bool active;           // allarme acceso
    uint32_t lost_s;       // secondi senza RTK fisso (0 se non perso)
    int quality;           // ultima qualita' (0-8 o RTK_ALARM_NO_DATA)
} rtk_alarm_out_t;

// Stato iniziale (mai stato in RTK fisso).
void rtk_alarm_reset(rtk_alarm_t *a);

// Un passo della macchina a stati: quality = qualita' attuale della GGA o
// RTK_ALARM_NO_DATA, now_us = tempo monotono in microsecondi, threshold_s =
// secondi dell'impostazione rtk_lost_alarm_s (0 = allarme spento). Si puo'
// chiamare piu' volte con la stessa lettura (es. ogni 100 ms).
rtk_alarm_out_t rtk_alarm_step(rtk_alarm_t *a, int quality, int64_t now_us, uint32_t threshold_s);

// Scritta breve (max 8 caratteri, maiuscole) della qualita' per il display
// e il pannello: "FLOAT", "DGPS", "NO FIX", "NO DATI", "AUTONOMA"...
const char *rtk_alarm_quality_label(int quality);
