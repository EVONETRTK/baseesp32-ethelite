#include "rtk_alarm.h"

#include <string.h>

void rtk_alarm_reset(rtk_alarm_t *a)
{
    memset(a, 0, sizeof(*a));
    a->last_quality = RTK_ALARM_NO_DATA;
}

rtk_alarm_out_t rtk_alarm_step(rtk_alarm_t *a, int quality, int64_t now_us, uint32_t threshold_s)
{
    rtk_alarm_out_t out = { .active = false, .lost_s = 0, .quality = quality };
    a->last_quality = quality;

    if (quality == 4) {
        // RTK fisso: niente perdita, l'allarme si spegne.
        a->had_fixed = true;
        a->lost = false;
        return out;
    }
    if (!a->had_fixed) {
        return out; // mai stato in RTK fisso dall'avvio: niente da perdere
    }
    if (!a->lost) {
        a->lost = true;
        a->lost_since_us = now_us;
    }
    int64_t dt = now_us - a->lost_since_us;
    if (dt < 0) {
        // Tempo che torna indietro (non dovrebbe succedere): si conta da qui.
        dt = 0;
        a->lost_since_us = now_us;
    }
    out.lost_s = (uint32_t) (dt / 1000000);
    // threshold_s = 0: spento. Il conto si tiene comunque, cosi' accendendolo
    // durante una perdita l'allarme parte dai secondi gia' passati.
    out.active = threshold_s > 0 && dt >= (int64_t) threshold_s * 1000000;
    return out;
}

const char *rtk_alarm_quality_label(int quality)
{
    switch (quality) {
    case RTK_ALARM_NO_DATA: return "NO DATI";
    case 0:                 return "NO FIX";
    case 1:                 return "AUTONOMA";
    case 2:                 return "DGPS";
    case 3:                 return "PPS";
    case 4:                 return "FIX";
    case 5:                 return "FLOAT";
    case 6:                 return "STIMATA";
    default:                return "ALTRO";
    }
}
