// Prove di main/base_selfpos.c (controllo dello spostamento dell'antenna):
// funzioni di calcolo pure e assemblaggio dei frame UBX.
//
// Riferimenti calcolati in Python scrivendo le formule direttamente dall'ICD
// (IS-GPS-200: Klobuchar, figura 20-4; posizione del satellite, tabella
// 20-IV): tests/host/strumenti/riferimenti.py, sezioni "Klobuchar" ed
// "Effemeride". L'effemeride e' inventata ma plausibile (orbita GPS): non
// si e' trovata un'effemeride pubblicata con la posizione calcolata da una
// fonte ufficiale, quindi la prova controlla che il firmware coincida con
// un'implementazione indipendente dell'ICD, piu' proprieta' fisiche (raggio
// dell'orbita).
#include "prove.h"
#include "../../main/etrf.c"
#include "../../main/base_selfpos.c"

// --- sostituti -----------------------------------------------------------------
static base_monitor_status_t s_bm;
base_monitor_status_t base_monitor_get_status(void) { return s_bm; }
void settings_get_into(app_settings_t *out) { memset(out, 0, sizeof(*out)); }

static void avvia(void)
{
    static bool fatto;
    if (!fatto) {
        base_selfpos_start(true, false);
        fatto = true;
    }
}

PROVA(lettura_dei_bit_con_e_senza_segno)
{
    const uint8_t b[] = { 0xA5, 0x0F, 0xF0, 0x80, 0x00, 0x01 };
    VERIFICA_INT(getbitu(b, 0, 8), 0xA5);
    VERIFICA_INT(getbitu(b, 4, 8), 0x50);
    VERIFICA_INT(getbitu(b, 12, 8), 0xFF);
    VERIFICA_INT(getbits(b, 12, 8), -1);
    VERIFICA_INT(getbits(b, 24, 8), -128);
    VERIFICA_INT(getbits(b, 25, 23), 1);
    VERIFICA_INT(getbitu(b, 0, 32), 0xA50FF080u);
    VERIFICA_INT(getbits(b, 0, 32), (int32_t) 0xA50FF080u);
    uint8_t c[4] = { 0 };
    setbit(c, 0, 1); setbit(c, 9, 1); setbit(c, 31, 1);
    VERIFICA_INT(c[0], 0x80); VERIFICA_INT(c[1], 0x40); VERIFICA_INT(c[3], 0x01);
    setbit(c, 0, 0);
    VERIFICA_INT(c[0], 0x00);
}

PROVA(crc24q_valore_di_controllo_standard)
{
    VERIFICA_INT(crc24q((const uint8_t *) "123456789", 9), 0xCDE703);
}

PROVA(differenza_di_tempo_dentro_la_settimana)
{
    VERIFICA_VICINO(dt_wrap(100.0), 100.0, 0);
    VERIFICA_VICINO(dt_wrap(604800.0 - 10.0), -10.0, 1e-9);
    VERIFICA_VICINO(dt_wrap(-604800.0 + 10.0), 10.0, 1e-9);
}

static eph_t eph_prova(void)
{
    eph_t e = { 0 };
    e.valid = true;
    e.mu = MU_GPS;
    e.toe = 388800.0; e.toc = 388800.0;
    e.f0 = 1.5e-5; e.f1 = -2.0e-12; e.f2 = 0.0;
    e.M0 = 0.8; e.deln = 4.5e-9; e.e = 0.012; e.sqrtA = 5153.65;
    e.OMG0 = -1.2; e.i0 = 0.96; e.omg = 0.5; e.OMGd = -8.0e-9; e.idot = 2.0e-10;
    e.cuc = 1.0e-6; e.cus = 8.0e-6; e.crc = 200.0; e.crs = 20.0; e.cic = 5.0e-8; e.cis = -3.0e-8;
    return e;
}

PROVA(posizione_satellite_come_icd_in_python)
{
    eph_t e = eph_prova();
    const struct { double t, x, y, z, dts; } atteso[] = {
        { 388800.0, -15908787.3874, 2091852.2097, 20888369.1440, 1.498012449227151e-05 },
        { 390600.0, -14890688.8000, -2779741.0264, 21631217.1779, 1.497225734253016e-05 },
        { 381800.0, -21021078.0982, 14607617.1925, 5811066.9691, 1.502009484879586e-05 },
    };
    for (size_t i = 0; i < 3; i++) {
        double xs[3], dts;
        VERIFICA(sat_pos(&e, atteso[i].t, xs, &dts));
        VERIFICA_VICINO(xs[0], atteso[i].x, 0.001);
        VERIFICA_VICINO(xs[1], atteso[i].y, 0.001);
        VERIFICA_VICINO(xs[2], atteso[i].z, 0.001);
        VERIFICA_VICINO(dts, atteso[i].dts, 1e-15);
        // raggio dell'orbita: A (1 - e cos E) +- correzioni, tra 25 900 e 27 300 km
        double r = sqrt(xs[0] * xs[0] + xs[1] * xs[1] + xs[2] * xs[2]);
        VERIFICA(r > 2.59e7 && r < 2.73e7);
    }
}

PROVA(effemeride_troppo_vecchia_o_assurda_rifiutata)
{
    eph_t e = eph_prova();
    double xs[3], dts;
    VERIFICA(!sat_pos(&e, 388800.0 + 4 * 3600.0 + 1, xs, &dts));
    // cambio di settimana: toe alla fine della settimana, t all'inizio della seguente
    eph_t w = eph_prova();
    w.toe = w.toc = 604000.0;
    VERIFICA(sat_pos(&w, 100.0, xs, &dts)); // tk = +900 s
    e.sqrtA = 100.0; // A = 1e4 m
    VERIFICA(!sat_pos(&e, 388800.0, xs, &dts));
}

PROVA(klobuchar_come_icd_in_python)
{
    const double a[4] = { 1.1176e-08, 7.4506e-09, -5.9605e-08, -5.9605e-08 };
    const double b[4] = { 90112.0, 0.0, -196608.0, -65536.0 };
    for (int i = 0; i < 4; i++) { s_ion[i] = a[i]; s_ion[4 + i] = b[i]; }
    s_ion_ok = true;
    const struct { double t, lat, lon, az, el, m; } c[] = {
        { 46800.0, 41.0, 16.5, 30.0, 45.0, 5.485656117 },
        { 7200.0, 41.0, 16.5, 200.0, 20.0, 3.261779218 },
        { 45000.0, 45.5, 9.2, 0.0, 90.0, 3.756895985 },
        { 45000.0, 41.0, 16.5, 120.0, 5.0, 13.752667461 },
    };
    const double r = SC_PI / 180.0;
    for (size_t i = 0; i < sizeof(c) / sizeof(c[0]); i++) {
        double geo[3] = { c[i].lat * r, c[i].lon * r, 400.0 };
        VERIFICA_VICINO(klobuchar(c[i].t, geo, c[i].az * r, c[i].el * r), c[i].m, 1e-6);
    }
    double geo[3] = { 41.0 * r, 16.5 * r, 0 };
    VERIFICA_VICINO(klobuchar(45000.0, geo, 0, -0.1), 0, 0); // sotto l'orizzonte
    s_ion_ok = false;
    VERIFICA_VICINO(klobuchar(45000.0, geo, 0, 1.0), 0, 0); // senza parametri
}

PROVA(geodetiche_ecef_andata_e_ritorno_e_direzioni_locali)
{
    const double r = SC_PI / 180.0;
    double geo[3] = { 41.0 * r, 16.5 * r, 450.0 }, p[3], g2[3];
    geo2ecef(geo, p);
    ecef2geo(p, g2);
    VERIFICA_VICINO(g2[0], geo[0], 1e-12);
    VERIFICA_VICINO(g2[1], geo[1], 1e-12);
    VERIFICA_VICINO(g2[2], geo[2], 1e-4);
    // un punto 10 m piu' in alto: in ENU e' (0, 0, 10)
    double su[3] = { geo[0], geo[1], 460.0 }, p2[3], d[3], enu[3];
    geo2ecef(su, p2);
    for (int k = 0; k < 3; k++) d[k] = p2[k] - p[k];
    to_enu(geo, d, enu);
    VERIFICA_VICINO(enu[0], 0, 1e-6);
    VERIFICA_VICINO(enu[1], 0, 1e-6);
    VERIFICA_VICINO(enu[2], 10.0, 1e-6);
    // spostamento verso nord: componente nord positiva
    double nord[3] = { geo[0] + 1e-6, geo[1], 450.0 };
    geo2ecef(nord, p2);
    for (int k = 0; k < 3; k++) d[k] = p2[k] - p[k];
    to_enu(geo, d, enu);
    VERIFICA(enu[1] > 6.3 && enu[1] < 6.4); // 1e-6 rad ~ 6,36 m
    VERIFICA_VICINO(enu[0], 0, 1e-6);
}

PROVA(ritardo_troposferico_plausibile)
{
    const double r = SC_PI / 180.0;
    double zen = tropo(0, 90 * r);
    VERIFICA(zen > 2.2 && zen < 2.6); // ~2,3-2,4 m allo zenit sul livello del mare
    VERIFICA(tropo(2000, 90 * r) < zen); // meno atmosfera in quota
    VERIFICA_VICINO(tropo(0, 30 * r), zen * 2.0, 0.01); // 1/sin(30)
    VERIFICA_VICINO(tropo(0, 0), 0, 0);
    VERIFICA_VICINO(tropo(20000, 1.0), 0, 0);
}

PROVA(sistema_lineare_risolto_e_singolare_rifiutato)
{
    // 2x + y - z = 8; -3x - y + 2z = -11; -2x + y + 2z = -3 -> (2, 3, -1)
    double N[5][6] = { { 2, 1, -1, 8 }, { -3, -1, 2, -11 }, { -2, 1, 2, -3 } };
    double x[5];
    VERIFICA(solve(N, 3, x));
    VERIFICA_VICINO(x[0], 2, 1e-12);
    VERIFICA_VICINO(x[1], 3, 1e-12);
    VERIFICA_VICINO(x[2], -1, 1e-12);
    double S[5][6] = { { 1, 2, 3 }, { 2, 4, 6 } };
    VERIFICA(!solve(S, 2, x));
}

PROVA(mediana_pari_e_dispari)
{
    avvia();
    const float d[] = { 5, 1, 4, 2, 3 };
    VERIFICA_VICINO(median(d, 5), 3, 0);
    VERIFICA_VICINO(median(d, 4), 3, 0); // 5 1 4 2 -> (2+4)/2
    VERIFICA_VICINO(median(d, 1), 5, 0);
}

PROVA(posizione_trasmessa_etrf2000_riportata_in_itrf)
{
    // Coordinate fisse in ETRF2000: riportate in ITRF2020 devono ridare le
    // stesse coordinate se riconvertite in ETRF2000 (inversa al primo ordine,
    // errore di frazioni di millimetro).
    avvia();
    s_fixed_etrf = true;
    double geo[3] = { 41.0 * SC_PI / 180.0, 16.5 * SC_PI / 180.0, 450.0 }, raw[3], itrf[3];
    geo2ecef(geo, raw);
    ref_to_itrf(raw, itrf);
    double g[3];
    ecef2geo(itrf, g);
    double lat = g[0] * 180.0 / SC_PI, lon = g[1] * 180.0 / SC_PI, h = g[2];
    double ep = etrf_decimal_year(time(NULL));
    etrf_itrf2020_to_etrf2000(&lat, &lon, &h, ep < 2024.0 ? 2026.8 : ep);
    double g3[3] = { lat * SC_PI / 180.0, lon * SC_PI / 180.0, h }, back[3];
    geo2ecef(g3, back);
    double d = sqrt((back[0] - raw[0]) * (back[0] - raw[0]) + (back[1] - raw[1]) * (back[1] - raw[1]) +
                    (back[2] - raw[2]) * (back[2] - raw[2]));
    VERIFICA(d < 0.001);
    double sposta = sqrt((itrf[0] - raw[0]) * (itrf[0] - raw[0]) + (itrf[1] - raw[1]) * (itrf[1] - raw[1]) +
                         (itrf[2] - raw[2]) * (itrf[2] - raw[2]));
    VERIFICA(sposta > 0.9 && sposta < 1.2);
    s_fixed_etrf = false;
    ref_to_itrf(raw, itrf);
    VERIFICA_VICINO(itrf[0], raw[0], 0);
}

// --- assemblaggio dei frame UBX in base_selfpos_feed -------------------------
static size_t ubx(uint8_t *f, uint8_t cls, uint8_t id, uint16_t n)
{
    f[0] = 0xB5; f[1] = 0x62; f[2] = cls; f[3] = id; f[4] = (uint8_t) n; f[5] = (uint8_t) (n >> 8);
    for (int i = 0; i < n; i++) f[6 + i] = (uint8_t) (i * 13 + 0xB5);
    uint8_t a = 0, b = 0;
    for (int i = 2; i < 6 + n; i++) { a += f[i]; b += a; }
    f[6 + n] = a;
    f[7 + n] = b;
    return 8u + n;
}

static int in_coda(void)
{
    static uint8_t out[6 + MAX_PAYLOAD + 2];
    int n = 0;
    while (xMessageBufferReceive(s_queue, out, sizeof(out), 0) > 0) n++;
    return n;
}

PROVA(frame_rawx_e_sfrbx_messi_in_coda_gli_altri_no)
{
    avvia();
    in_coda();
    static uint8_t f[6000];
    size_t n = ubx(f, 0x02, 0x15, 16 + 32 * 20); // RXM-RAWX
    base_selfpos_feed(f, n);
    n = ubx(f, 0x02, 0x13, 48); // RXM-SFRBX
    base_selfpos_feed(f, n);
    n = ubx(f, 0x01, 0x07, 92); // NAV-PVT: non serve
    base_selfpos_feed(f, n);
    VERIFICA_INT(in_coda(), 2);
}

PROVA(frame_spezzato_a_byte_e_preceduto_da_b5_doppio)
{
    avvia();
    in_coda();
    static uint8_t f[6000];
    const uint8_t pre[] = { 0x00, 0xB5, 0xB5 };
    base_selfpos_feed(pre, 2); // "00 B5"
    size_t n = ubx(f, 0x02, 0x15, 16 + 32 * 3);
    // il frame comincia con un altro B5: "B5 B5 62 ..."
    for (size_t i = 0; i < n; i++) base_selfpos_feed(&f[i], 1);
    VERIFICA_INT(in_coda(), 1);
}

PROVA(frame_con_checksum_sbagliato_o_troppo_lungo_scartato)
{
    avvia();
    in_coda();
    static uint8_t f[6000];
    size_t n = ubx(f, 0x02, 0x15, 16 + 32 * 2);
    f[20] ^= 0x01;
    base_selfpos_feed(f, n);
    VERIFICA_INT(in_coda(), 0);
    n = ubx(f, 0x02, 0x15, MAX_PAYLOAD + 1);
    base_selfpos_feed(f, n);
    VERIFICA_INT(in_coda(), 0);
    n = ubx(f, 0x02, 0x15, 16); // dopo, un frame buono passa
    base_selfpos_feed(f, n);
    VERIFICA_INT(in_coda(), 1);
}

int main(void)
{
    ESEGUI(lettura_dei_bit_con_e_senza_segno);
    ESEGUI(crc24q_valore_di_controllo_standard);
    ESEGUI(differenza_di_tempo_dentro_la_settimana);
    ESEGUI(posizione_satellite_come_icd_in_python);
    ESEGUI(effemeride_troppo_vecchia_o_assurda_rifiutata);
    ESEGUI(klobuchar_come_icd_in_python);
    ESEGUI(geodetiche_ecef_andata_e_ritorno_e_direzioni_locali);
    ESEGUI(ritardo_troposferico_plausibile);
    ESEGUI(sistema_lineare_risolto_e_singolare_rifiutato);
    ESEGUI(mediana_pari_e_dispari);
    ESEGUI(posizione_trasmessa_etrf2000_riportata_in_itrf);
    ESEGUI(frame_rawx_e_sfrbx_messi_in_coda_gli_altri_no);
    ESEGUI(frame_spezzato_a_byte_e_preceduto_da_b5_doppio);
    ESEGUI(frame_con_checksum_sbagliato_o_troppo_lungo_scartato);
    return prove_fine();
}
