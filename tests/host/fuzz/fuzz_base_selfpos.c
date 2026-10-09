// Fuzzing di main/base_selfpos.c (controllo dello spostamento dell'antenna):
// assemblaggio dei frame UBX dal flusso grezzo, decodifica di RXM-SFRBX
// (GPS LNAV: decode_lnav, Galileo I/NAV: decode_inav) e di RXM-RAWX,
// posizione singola, finestre di 30 minuti e confronto con il 1005.
//
// L'input e' una sequenza di "comandi" (primo byte = tipo), cosi' il fuzzer
// arriva in fondo alla catena invece di fermarsi ai controlli:
//   0 byte grezzi nel flusso (lunghezza, byte)
//   1 RXM-SFRBX GPS con 10 parole dall'input (opzioni: preambolo 0x8B,
//     numero di sottoframe, IODE coerenti forzati)
//   2 RXM-SFRBX Galileo con 8 parole dall'input (opzioni: pagina pari +
//     dispari, tipo di parola, CRC24Q ricalcolato, IOD e SVID coerenti)
//   3 RXM-RAWX con intestazione e misure dall'input
//   4 posizione trasmessa nel 1005 (dal base_monitor)
//   5 tempo che passa
//   6 effemeridi "sintetiche" di satelliti visibili (messe direttamente in
//     memoria come se decodificate: servono a raggiungere il calcolo)
//   7 epoche RAWX "sintetiche": pseudodistanze calcolate per una posizione
//     vera vicina alla trasmessa (rumore, spostamento, satelliti fuori
//     scala dall'input), una ogni 5 s, fino a 255 per comando
// Ogni frame passa da base_selfpos_feed (checksum UBX giusto, a pezzi di
// misura casuale), poi la coda viene svuotata come fa il task.
// SORGENTI: etrf.c
#include "motore.h"
#include "aiuti.h"
#include "../../../main/base_selfpos.c"

// --- sostituti ---------------------------------------------------------------------
static base_monitor_status_t s_bm;
base_monitor_status_t base_monitor_get_status(void) { return s_bm; }
static float s_soglia;
void settings_get_into(app_settings_t *out)
{
    memset(out, 0, sizeof(*out));
    out->base_drift_threshold_m = s_soglia;
}
void host_time_advance_us(int64_t us);

// --- lettura dell'input ------------------------------------------------------------
static const uint8_t *g_d;
static size_t g_n, g_i;
static uint8_t rd8(void) { return g_i < g_n ? g_d[g_i++] : 0; }
static void rd(void *out, size_t k)
{
    for (size_t j = 0; j < k; j++) ((uint8_t *) out)[j] = rd8();
}

// --- invio dei frame -------------------------------------------------------------------
static void pompa(void)
{
    size_t n;
    while ((n = xMessageBufferReceive(s_queue, s_work, 6 + MAX_PAYLOAD + 2, 0)) > 0) {
        if (n >= 8) process_frame(s_work, n);
    }
}

static void flusso(const uint8_t *f, size_t n)
{
    size_t i = 0;
    while (i < n) {
        size_t k = 1 + fuzz_caso() % 700;
        if (k > n - i) k = n - i;
        uint8_t *b = fuzz_buffer(k);
        memcpy(b, f + i, k);
        base_selfpos_feed(b, k);
        fuzz_zone_azzera();
        i += k;
        pompa(); // il task svuota la coda mentre arrivano i dati
    }
}

static uint8_t g_frame[6 + MAX_PAYLOAD + 2 + 64];

static void manda(uint8_t id, const uint8_t *pl, size_t n)
{
    if (n > MAX_PAYLOAD + 16) n = MAX_PAYLOAD + 16;
    uint8_t *f = g_frame;
    f[0] = 0xB5; f[1] = 0x62; f[2] = 0x02; f[3] = id; f[4] = (uint8_t) n; f[5] = (uint8_t) (n >> 8);
    memcpy(f + 6, pl, n);
    aiuto_ubx_ck(f + 2, 4 + n, &f[6 + n], &f[7 + n]);
    flusso(f, 8 + n);
}

static void bits_set(uint8_t *b, int pos, int len, uint32_t v)
{
    for (int k = 0; k < len; k++) setbit(b, pos + k, (int) ((v >> (len - 1 - k)) & 1));
}

// --- comandi -----------------------------------------------------------------------------
static void cmd_sfrbx_gps(void)
{
    uint8_t sv = rd8(), opz = rd8();
    uint8_t b[30];
    rd(b, 30);
    if (opz & 1) b[0] = 0x8B;                              // preambolo
    if (opz & 2) bits_set(b, 43, 3, 1 + (opz >> 2) % 4);   // sottoframe 1..4
    if (opz & 0x10) bits_set(b, 50, 6, 56);                // pagina 56 (Klobuchar)
    if (opz & 0x20) {                                      // IODE/IODC coerenti tra i sottoframi
        uint8_t iode = (uint8_t) (opz * 37);
        int id = (int) getbitu(b, 43, 3);
        if (id == 1) bits_set(b, 48 + 10 + 2 + 4 + 6 + 2 + 1 + 87 + 8, 8, iode);
        if (id == 2) bits_set(b, 48, 8, iode);
        if (id == 3) bits_set(b, 48 + 16 + 32 + 16 + 32 + 16 + 32 + 24, 8, iode);
    }
    uint8_t pl[8 + 40] = { 0, sv, 0, 0, 10, 0, 2, 0 };
    for (int i = 0; i < 10; i++) {
        uint32_t w = ((uint32_t) b[i * 3] << 16 | (uint32_t) b[i * 3 + 1] << 8 | b[i * 3 + 2]) << 6;
        if (opz & 0x40) w |= rd8() & 0x3F; // bit di parita' dall'input
        memcpy(pl + 8 + 4 * i, &w, 4);
    }
    if (opz & 0x80) pl[4] = rd8(); // numero di parole sbagliato
    manda(0x13, pl, 8 + 4u * (pl[4] <= 16 ? pl[4] : 10));
}

static void cmd_sfrbx_gal(void)
{
    uint8_t sv = rd8(), opz = rd8();
    uint8_t p[32];
    rd(p, 32);
    if (opz & 1) { // pagina pari (bit 0 = 0) + dispari (bit 128 = 1), niente allarme
        setbit(p, 0, 0); setbit(p, 1, 0); setbit(p, 128, 1); setbit(p, 129, 0);
    }
    if (opz & 2) { // tipo di parola 1..5 nei primi 6 bit dei dati (bit 2.. della pagina)
        uint32_t t = 1 + (opz >> 2) % 5;
        for (int k = 0; k < 6; k++) setbit(p, 2 + k, (int) ((t >> (5 - k)) & 1));
        if (opz & 0x20) { // IOD uguale per tutte le parole (bit 6..15 dei dati)
            for (int k = 0; k < 10; k++) setbit(p, 2 + 6 + k, (int) ((0x155u >> (9 - k)) & 1));
        }
        if ((opz & 0x40) && t == 4) { // SVID = sv (bit 16..21 dei dati)
            for (int k = 0; k < 6; k++) setbit(p, 2 + 16 + k, (int) ((sv >> (5 - k)) & 1));
        }
    }
    if (opz & 0x10) { // CRC24Q come lo calcola il ricevitore
        uint8_t cb[25] = { 0 };
        for (int i = 0; i < 114; i++) setbit(cb, 4 + i, (int) getbitu(p, i, 1));
        for (int i = 0; i < 82; i++) setbit(cb, 118 + i, (int) getbitu(p, 128 + i, 1));
        uint32_t c = crc24q(cb, 25);
        for (int k = 0; k < 24; k++) setbit(p, 128 + 82 + k, (int) ((c >> (23 - k)) & 1));
    }
    uint8_t pl[8 + 32] = { 2, sv, 0, 0, 8, 0, 2, 0 };
    for (int i = 0; i < 8; i++) {
        uint32_t w = (uint32_t) p[i * 4] << 24 | (uint32_t) p[i * 4 + 1] << 16 | (uint32_t) p[i * 4 + 2] << 8 | p[i * 4 + 3];
        memcpy(pl + 8 + 4 * i, &w, 4);
    }
    manda(0x13, pl, sizeof(pl));
}

static uint8_t g_pl[MAX_PAYLOAD + 64];

static void cmd_rawx(void)
{
    uint8_t nm = rd8(), opz = rd8();
    size_t n = 16 + 32u * (nm % 48);
    if (opz & 1) n = rd8() | ((size_t) rd8() << 8); // lunghezza qualsiasi
    if (n > MAX_PAYLOAD + 8) n = MAX_PAYLOAD + 8;
    rd(g_pl, n < 64 ? n : 64);
    for (size_t j = 64; j < n; j++) g_pl[j] = rd8();
    g_pl[11] = (opz & 2) ? g_pl[11] : nm;
    manda(0x15, g_pl, n);
}

static void cmd_riferimento(void)
{
    uint8_t opz = rd8();
    s_bm.last_position_set = opz & 1;
    if (opz & 2) {
        rd(&s_bm.last_ecef_x_m, 8);
        rd(&s_bm.last_ecef_y_m, 8);
        rd(&s_bm.last_ecef_z_m, 8);
    } else {
        double geo[3] = { (41.0 + (int8_t) rd8() * 0.01) * SC_PI / 180.0, (16.5 + (int8_t) rd8() * 0.01) * SC_PI / 180.0,
                          450.0 + (int8_t) rd8() };
        double p[3];
        geo2ecef(geo, p);
        s_bm.last_ecef_x_m = p[0];
        s_bm.last_ecef_y_m = p[1];
        s_bm.last_ecef_z_m = p[2];
    }
    s_fixed_etrf = (opz & 4) != 0;
    s_soglia = (float) ((int8_t) rd8()) / 4.0f;
}

// Effemeridi sintetiche: GPS (o Galileo) visibili da 41 N 16,5 E.
static const double TOW0 = 388800.0;
static const uint16_t SETTIMANA = 2400;
static double g_tow = TOW0;
static int g_epoche; // epoche sintetiche in questo input

static void cmd_effemeridi(void)
{
    uint8_t quanti = rd8() % 16, opz = rd8();
    bool gal = opz & 1;
    double geo[3] = { 41.0 * SC_PI / 180.0, 16.5 * SC_PI / 180.0, 450.0 }, rx[3];
    geo2ecef(geo, rx);
    s_now_abs = SETTIMANA * 604800.0 + g_tow;
    int messi = 0;
    for (int j = 0; j < 64 && messi < quanti; j++) {
        eph_t e = { 0 };
        e.valid = !(opz & 2) || (fuzz_caso() & 7);
        e.mu = gal ? MU_GAL : MU_GPS;
        e.toe = e.toc = TOW0;
        e.f0 = 1e-5 * ((int8_t) fuzz_caso()) / 128.0;
        e.f1 = -2e-12;
        e.M0 = j * 1.37;
        e.deln = 4.5e-9;
        e.e = 0.001 + (fuzz_caso() % 32) * 0.001;
        e.sqrtA = gal ? 5440.6 : 5153.65;
        e.OMG0 = -SC_PI + j * 0.61;
        e.i0 = gal ? 0.977 : 0.96;
        e.omg = 0.5;
        e.OMGd = -8e-9;
        e.idot = 2e-10;
        e.tgd = (opz & 4) ? ((int8_t) fuzz_caso()) * 1e-9 : 0;
        e.t_dec = s_now_abs;
        double xs[3], dts, d[3], enu[3];
        if (!sat_pos(&e, g_tow, xs, &dts)) continue;
        for (int k = 0; k < 3; k++) d[k] = xs[k] - rx[k];
        to_enu(geo, d, enu);
        double r = sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (asin(enu[2] / r) < 15.0 * SC_PI / 180.0) continue;
        int sv = 1 + messi + (gal ? 0 : 0);
        if (gal) s_gal[sv] = e; else s_gps[sv] = e;
        messi++;
    }
    if (opz & 8) { // parametri Klobuchar
        const double a[4] = { 1.1176e-08, 7.4506e-09, -5.9605e-08, -5.9605e-08 };
        const double b[4] = { 90112.0, 0.0, -196608.0, -65536.0 };
        for (int i = 0; i < 4; i++) { s_ion[i] = a[i]; s_ion[4 + i] = b[i]; }
        s_ion_ok = true;
    }
}

// Epoche sintetiche: pseudodistanze come le misurerebbe un ricevitore in
// "vero" (riferimento + spostamento), con orologio, troposfera, ionosfera.
static void cmd_epoche(void)
{
    uint8_t k = rd8(), rumore = rd8(), opz = rd8();
    // al massimo 420 epoche per input (35 minuti: una finestra e poco piu'):
    // ogni epoca e' un calcolo completo, oltre il fuzzing rallenta troppo
    if (g_epoche + k > 420) k = (uint8_t) (g_epoche < 420 ? 420 - g_epoche : 0);
    g_epoche += k;
    double sp[3] = { (int8_t) rd8() * 0.5, (int8_t) rd8() * 0.5, (int8_t) rd8() * 0.5 }; // spostamento ENU (m)
    double ref[3] = { s_bm.last_ecef_x_m, s_bm.last_ecef_y_m, s_bm.last_ecef_z_m };
    double geo[3];
    if (!(ref[0] * ref[0] + ref[1] * ref[1] + ref[2] * ref[2] > 1e12)) { // riferimento assente: 41 N 16,5 E
        double g0[3] = { 41.0 * SC_PI / 180.0, 16.5 * SC_PI / 180.0, 450.0 };
        geo2ecef(g0, ref);
    }
    ecef2geo(ref, geo);
    double sl = sin(geo[0]), cl = cos(geo[0]), so = sin(geo[1]), co = cos(geo[1]);
    double rx[3] = {
        ref[0] - so * sp[0] - sl * co * sp[1] + cl * co * sp[2],
        ref[1] + co * sp[0] - sl * so * sp[1] + cl * so * sp[2],
        ref[2] + cl * sp[1] + sl * sp[2],
    };
    double grx[3];
    ecef2geo(rx, grx);
    for (int ep = 0; ep < k; ep++) {
        g_tow += 5.0;
        host_time_advance_us(5000000);
        uint8_t *pl = g_pl;
        memset(pl, 0, 16);
        memcpy(pl, &g_tow, 8);
        pl[8] = (uint8_t) SETTIMANA;
        pl[9] = (uint8_t) (SETTIMANA >> 8);
        int nm = 0;
        for (int gal = 0; gal < 2; gal++) {
            for (int sv = 1; sv <= (gal ? GAL_N : GPS_N) && nm < 40; sv++) {
                const eph_t *e = gal ? &s_gal[sv] : &s_gps[sv];
                if (!e->valid) continue;
                double xs[3], dts, tau = 0.075;
                for (int it = 0; it < 3; it++) {
                    sat_pos(e, g_tow - tau, xs, &dts);
                    double r0 = sqrt((xs[0] - rx[0]) * (xs[0] - rx[0]) + (xs[1] - rx[1]) * (xs[1] - rx[1]) + (xs[2] - rx[2]) * (xs[2] - rx[2]));
                    double rot = OMGE * r0 / CLIGHT;
                    double sx = xs[0] * cos(rot) + xs[1] * sin(rot), sy = -xs[0] * sin(rot) + xs[1] * cos(rot);
                    double rho = sqrt((sx - rx[0]) * (sx - rx[0]) + (sy - rx[1]) * (sy - rx[1]) + (xs[2] - rx[2]) * (xs[2] - rx[2]));
                    tau = rho / CLIGHT;
                    if (it == 2) {
                        double d[3] = { sx - rx[0], sy - rx[1], xs[2] - rx[2] }, enu[3];
                        to_enu(grx, d, enu);
                        double el = asin(enu[2] / rho), az = atan2(enu[0], enu[1]);
                        double corr = tropo(grx[2], el) + klobuchar(g_tow, grx, az, el);
                        double clk = gal ? 120.0 : 35.0;
                        double pr = rho + clk - CLIGHT * (dts - e->tgd) + corr + ((int) (fuzz_caso() % 2001) - 1000) * rumore * 1e-3;
                        if ((opz & 1) && (fuzz_caso() % 16) == 0) pr += 150.0 + (fuzz_caso() % 1000); // misura fuori scala
                        uint8_t *q = pl + 16 + 32 * nm;
                        memset(q, 0, 32);
                        memcpy(q, &pr, 8);
                        q[20] = gal ? 2 : 0;
                        q[21] = (uint8_t) sv;
                        q[22] = (opz & 2) && gal ? 1 : 0;
                        q[26] = (opz & 4) ? (uint8_t) (fuzz_caso() % 50) : 42;
                        q[30] = 1;
                        nm++;
                    }
                }
            }
        }
        pl[11] = (uint8_t) nm;
        manda(0x15, pl, 16 + 32u * nm);
    }
}

// --- bersaglio ---------------------------------------------------------------------------
void bersaglio_avvio(void)
{
    base_selfpos_start(true, false);
    fuzz_imposta_max_len(6000);
    fuzz_imposta_limite_ms(5000);
}

static void azzera(void)
{
    s_pos = 0;
    s_need = 0;
    pompa();
    memset(s_gps, 0, (GPS_N + 1) * sizeof(eph_t));
    memset(s_gal, 0, (GAL_N + 1) * sizeof(eph_t));
    memset(s_gps_sub, 0, (GPS_N + 1) * sizeof(*s_gps_sub));
    memset(s_gal_word, 0, (GAL_N + 1) * sizeof(*s_gal_word));
    memset(s_gps_sub_ok, 0, sizeof(s_gps_sub_ok));
    memset(s_gal_word_ok, 0, sizeof(s_gal_word_ok));
    s_ion_ok = false;
    s_x_ok = false;
    s_now_abs = 0;
    s_last_tow = -1;
    s_ref_ok = false;
    memset(s_ref_raw, 0, sizeof(s_ref_raw));
    s_result_us = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    uint8_t st = s_st.state;
    memset(&s_st, 0, sizeof(s_st));
    s_st.state = st;
    s_st.window_len_s = WINDOW_S;
    xSemaphoreGive(s_mutex);
    reset_window();
    memset(&s_bm, 0, sizeof(s_bm));
    s_soglia = 0;
    s_fixed_etrf = false;
    g_tow = TOW0;
    g_epoche = 0;
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    azzera();
    g_d = d;
    g_n = n;
    g_i = 0;
    while (g_i < g_n) {
        switch (rd8() % 8) {
        case 0: {
            size_t k = rd8();
            if (k > g_n - g_i) k = g_n - g_i;
            flusso(g_d + g_i, k);
            g_i += k;
            break;
        }
        case 1: cmd_sfrbx_gps(); break;
        case 2: cmd_sfrbx_gal(); break;
        case 3: cmd_rawx(); break;
        case 4: cmd_riferimento(); break;
        case 5: host_time_advance_us((int64_t) rd8() * 60 * 1000000); break;
        case 6: cmd_effemeridi(); break;
        default: cmd_epoche(); break;
        }
    }
    pompa();
    base_selfpos_status_t st;
    base_selfpos_get_status(&st);
    FUZZ_VERIFICA(st.samples <= MAX_SAMPLES);
    static int dettagli = -1;
    if (dettagli < 0) dettagli = getenv("FUZZ_DETTAGLI") != NULL;
    if (dettagli) {
        printf("stato %u, satelliti %u, effemeridi %u+%u, campioni %u, finestre %u, risultato %d (E %.2f N %.2f U %.2f), "
               "allarme %d\n", st.state, st.sats, st.eph_gps, st.eph_gal, st.samples, (unsigned) st.windows,
               st.have_result, st.d_east_m, st.d_north_m, st.d_up_m, st.alarm);
    }
}

void bersaglio_semi(void)
{
    // 1) catena completa: riferimento, 9 GPS + Klobuchar, 2 x 200 epoche
    //    (oltre i 30 minuti: finestra chiusa e risultato)
    const uint8_t s1[] = { 4, 1, 0, 0, 0, 20,  6, 9, 8,  7, 200, 3, 0, 0, 0, 0,  7, 200, 3, 0, 0, 0, 0 };
    fuzz_seme(s1, sizeof(s1));
    // 2) con spostamento di 10 m a est e misure fuori scala, Galileo
    const uint8_t s2[] = { 4, 1, 1, 2, 3, 8,  6, 9, 9,  6, 7, 0,  7, 255, 5, 3, 20, 0, 0,  7, 255, 5, 3, 20, 0, 0 };
    fuzz_seme(s2, sizeof(s2));
    // 3) coordinate fisse ETRF2000, soglia piccola
    const uint8_t s3[] = { 4, 5, 0, 0, 0, 1,  6, 12, 8,  7, 250, 1, 0, 2, 2, 0,  7, 250, 1, 0, 2, 2, 0 };
    fuzz_seme(s3, sizeof(s3));
    // 4) sottoframi GPS e pagine Galileo con le opzioni di coerenza, RAWX
    uint8_t s4[400];
    size_t n = 0;
    for (int id = 0; id < 4; id++) {
        s4[n++] = 1; s4[n++] = 5; s4[n++] = (uint8_t) (0x23 | (id << 2));
        for (int i = 0; i < 30; i++) s4[n++] = (uint8_t) (i * 7 + id);
    }
    for (int t = 0; t < 5; t++) {
        s4[n++] = 2; s4[n++] = 11; s4[n++] = (uint8_t) (0x73 | ((t % 5) << 2));
        for (int i = 0; i < 32; i++) s4[n++] = (uint8_t) (i * 13 + t);
    }
    s4[n++] = 3; s4[n++] = 3; s4[n++] = 0;
    for (int i = 0; i < 64; i++) s4[n++] = (uint8_t) i;
    fuzz_seme(s4, n);
}
