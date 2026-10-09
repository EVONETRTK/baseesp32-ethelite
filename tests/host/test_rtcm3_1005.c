// Prove di main/rtcm3_1005.c: CRC24Q e lettura della posizione della base
// dai messaggi RTCM3 1005/1006.
//
// Frame costruiti in Python bit per bit (tests/host/strumenti/
// riferimenti.py, sezione "RTCM"), CRC24Q calcolato li'. Nota: il firmware
// non legge l'ID stazione (non serve per rilevare uno spostamento); qui si
// controlla solo che il frame lo contenga dove previsto dallo standard.
#include "prove.h"
#include "../../main/rtcm3_1005.c"

// 1005, stazione 2003, X 4626000.1234 Y 1370000.5678 Z 4162000.9012
static const uint8_t F1005[] = {
    0xD3, 0x00, 0x13, 0x3E, 0xD7, 0xD3, 0x03, 0x8A, 0xC5, 0x4F, 0x99, 0xD2, 0x03,
    0x30, 0x95, 0x7F, 0x2E, 0x09, 0xB0, 0xBE, 0xF0, 0x34, 0xF6, 0x3D, 0x42,
};
// 1006, stazione 4095, X -2700000.0001 Y -4300000.9999 Z 3850000.5000, altezza 1.5 m
static const uint8_t F1006[] = {
    0xD3, 0x00, 0x15, 0x3E, 0xEF, 0xFF, 0x03, 0xB9, 0xB6, 0xAC, 0xB1, 0xFF, 0x35, 0xFC,
    0xFF, 0xEA, 0xF1, 0x08, 0xF6, 0xC7, 0x74, 0x88, 0x3A, 0x98, 0x79, 0x17, 0xCC,
};
// 1077 finto e corto (contiene 0xD3 nel payload)
static const uint8_t F1077[] = {
    0xD3, 0x00, 0x09, 0x43, 0x51, 0x23, 0xAA, 0x55, 0xD3, 0x00, 0x13, 0x62, 0x0F, 0xA7, 0xF9,
};

static void verifica_1005(const rtcm3_position_t *p)
{
    VERIFICA_VICINO(p->ecef_x_m, 4626000.1234, 1e-6);
    VERIFICA_VICINO(p->ecef_y_m, 1370000.5678, 1e-6);
    VERIFICA_VICINO(p->ecef_z_m, 4162000.9012, 1e-6);
}

PROVA(crc24q_valore_di_controllo_standard)
{
    // CRC-24Q di "123456789" = 0xCDE703 (valore di controllo noto)
    uint32_t crc = 0;
    for (const char *c = "123456789"; *c; c++) crc = crc24q_update(crc, (uint8_t) *c);
    VERIFICA_INT(crc, 0xCDE703);
}

PROVA(frame_1005_contiene_id_stazione_2003)
{
    // bit 12..23 del payload
    unsigned id = ((unsigned) (F1005[4] & 0x0F) << 8) | F1005[5];
    VERIFICA_INT(id, 2003);
}

PROVA(frame_1005_decodificato)
{
    rtcm3_1005_init();
    rtcm3_position_t p = { 0 };
    VERIFICA(rtcm3_1005_feed(F1005, sizeof(F1005), &p));
    verifica_1005(&p);
}

PROVA(frame_1006_coordinate_negative)
{
    rtcm3_1005_init();
    rtcm3_position_t p = { 0 };
    VERIFICA(rtcm3_1005_feed(F1006, sizeof(F1006), &p));
    VERIFICA_VICINO(p.ecef_x_m, -2700000.0001, 1e-6);
    VERIFICA_VICINO(p.ecef_y_m, -4300000.9999, 1e-6);
    VERIFICA_VICINO(p.ecef_z_m, 3850000.5, 1e-6);
}

PROVA(frame_1005_a_un_byte_per_volta)
{
    rtcm3_1005_init();
    rtcm3_position_t p = { 0 };
    int trovati = 0;
    for (size_t i = 0; i < sizeof(F1005); i++) {
        if (rtcm3_1005_feed(&F1005[i], 1, &p)) {
            trovati++;
            VERIFICA_INT(i, sizeof(F1005) - 1); // solo all'ultimo byte del CRC
        }
    }
    VERIFICA_INT(trovati, 1);
    verifica_1005(&p);
}

PROVA(crc_sbagliato_rifiutato)
{
    uint8_t f[sizeof(F1005)];
    rtcm3_position_t p = { 0 };
    // un bit cambiato nella coordinata X
    memcpy(f, F1005, sizeof(f));
    f[9] ^= 0x10;
    rtcm3_1005_init();
    VERIFICA(!rtcm3_1005_feed(f, sizeof(f), &p));
    // un byte del CRC cambiato
    memcpy(f, F1005, sizeof(f));
    f[sizeof(f) - 1] ^= 0x01;
    rtcm3_1005_init();
    VERIFICA(!rtcm3_1005_feed(f, sizeof(f), &p));
    // dopo il frame rifiutato quello buono si legge ancora
    VERIFICA(rtcm3_1005_feed(F1005, sizeof(F1005), &p));
    verifica_1005(&p);
}

PROVA(altri_messaggi_saltati_e_vale_l_ultimo)
{
    uint8_t flusso[sizeof(F1077) + sizeof(F1006) + sizeof(F1077) + sizeof(F1005) + 4];
    size_t n = 0;
    flusso[n++] = 0x00; // spazzatura prima del primo preambolo
    flusso[n++] = 0x41;
    memcpy(flusso + n, F1077, sizeof(F1077)); n += sizeof(F1077);
    memcpy(flusso + n, F1006, sizeof(F1006)); n += sizeof(F1006);
    memcpy(flusso + n, F1077, sizeof(F1077)); n += sizeof(F1077);
    memcpy(flusso + n, F1005, sizeof(F1005)); n += sizeof(F1005);
    rtcm3_1005_init();
    rtcm3_position_t p = { 0 };
    VERIFICA(rtcm3_1005_feed(flusso, n, &p));
    verifica_1005(&p); // l'ultimo dei due trovati
}

PROVA(frame_lunghezza_zero_o_uno_ignorati)
{
    const uint8_t corti[] = { 0xD3, 0x00, 0x00, 0xD3, 0x00, 0x01 };
    rtcm3_1005_init();
    rtcm3_position_t p = { 0 };
    VERIFICA(!rtcm3_1005_feed(corti, sizeof(corti), &p));
    // lunghezza 1: il byte seguente e' trattato come ricerca del preambolo
    VERIFICA(rtcm3_1005_feed(F1005, sizeof(F1005), &p));
}

int main(void)
{
    ESEGUI(crc24q_valore_di_controllo_standard);
    ESEGUI(frame_1005_contiene_id_stazione_2003);
    ESEGUI(frame_1005_decodificato);
    ESEGUI(frame_1006_coordinate_negative);
    ESEGUI(frame_1005_a_un_byte_per_volta);
    ESEGUI(crc_sbagliato_rifiutato);
    ESEGUI(altri_messaggi_saltati_e_vale_l_ultimo);
    ESEGUI(frame_lunghezza_zero_o_uno_ignorati);
    return prove_fine();
}
