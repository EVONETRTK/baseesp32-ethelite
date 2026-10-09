// Prove di main/rtcm3_stats.c: conteggio dei byte per tipo di messaggio
// RTCM3 (diagnostica del pannello). Stessi frame di test_rtcm3_1005.c
// (costruiti in Python, tests/host/strumenti/riferimenti.py).
#include "prove.h"
#include "../../main/rtcm3_stats.c"

static const uint8_t F1005[] = {
    0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x03, 0x8A, 0xC5, 0x4F, 0x99, 0xD2, 0x03,
    0x30, 0x95, 0x7F, 0x2E, 0x09, 0xB0, 0xBE, 0xF0, 0x34, 0xF6, 0x3D, 0x42,
};
static const uint8_t F1077[] = {
    0xD3, 0x00, 0x09, 0x43, 0x51, 0x23, 0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62, 0x0F, 0xA7, 0xF9,
};

static uint32_t byte_di(uint16_t tipo)
{
    rtcm3_stat_entry_t e[32];
    size_t n = rtcm3_stats_get(e, 32);
    for (size_t i = 0; i < n; i++) {
        if (e[i].msg_type == tipo) return e[i].bytes;
    }
    return 0;
}

PROVA(byte_contati_per_tipo)
{
    rtcm3_stats_init();
    rtcm3_stats_feed(F1005, sizeof(F1005));
    rtcm3_stats_feed(F1077, sizeof(F1077));
    rtcm3_stats_feed(F1005, sizeof(F1005));
    VERIFICA_INT(byte_di(1005), 2 * sizeof(F1005));
    VERIFICA_INT(byte_di(1077), sizeof(F1077));
    rtcm3_stat_entry_t e[4];
    VERIFICA_INT(rtcm3_stats_get(e, 4), 2);
    VERIFICA_INT(rtcm3_stats_get(e, 1), 1); // rispetta il massimo richiesto
}

PROVA(frame_spezzato_contato_una_volta)
{
    rtcm3_stats_init();
    for (size_t i = 0; i < sizeof(F1077); i++) rtcm3_stats_feed(&F1077[i], 1);
    VERIFICA_INT(byte_di(1077), sizeof(F1077));
    // frame a meta': non ancora contato
    rtcm3_stats_feed(F1005, 10);
    VERIFICA_INT(byte_di(1005), 0);
    rtcm3_stats_feed(F1005 + 10, sizeof(F1005) - 10);
    VERIFICA_INT(byte_di(1005), sizeof(F1005));
}

PROVA(tabella_piena_tipi_nuovi_ignorati_senza_danni)
{
    rtcm3_stats_init();
    // 30 tipi diversi (frame finti senza CRC valido: le statistiche non lo controllano)
    for (int t = 0; t < 30; t++) {
        uint16_t tipo = (uint16_t) (1001 + t);
        uint8_t f[9] = { 0xD3, 0x00, 0x03, (uint8_t) (tipo >> 4), (uint8_t) ((tipo & 0x0F) << 4), 0x00, 0, 0, 0 };
        rtcm3_stats_feed(f, sizeof(f));
    }
    rtcm3_stat_entry_t e[64];
    VERIFICA_INT(rtcm3_stats_get(e, 64), MAX_TRACKED_TYPES);
    VERIFICA_INT(byte_di(1001), 9);
    VERIFICA_INT(byte_di(1030), 0); // oltre i 24
}

int main(void)
{
    ESEGUI(byte_contati_per_tipo);
    ESEGUI(frame_spezzato_contato_una_volta);
    ESEGUI(tabella_piena_tipi_nuovi_ignorati_senza_danni);
    return prove_fine();
}
