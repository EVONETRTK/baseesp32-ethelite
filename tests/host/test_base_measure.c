// Prove di main/base_measure.c: media incrementale (Welford), scarto dei
// campioni lontani dalla media, dispersione e salti della misura HAS.
// I task della misura (cicli infiniti) non si provano qui.
#include "prove.h"
#include "../../main/etrf.c"
#include "../../main/base_measure.c"

// --- sostituti dei moduli usati da base_measure.c -------------------------
void settings_peek(void (*fn)(const app_settings_t *s, void *ctx), void *ctx)
{
    static app_settings_t s;
    fn(&s, ctx);
}
esp_err_t settings_update(void (*fn)(app_settings_t *s, void *ctx), void *ctx)
{
    static app_settings_t s;
    fn(&s, ctx);
    return ESP_OK;
}
void sys_stats_note_restart_reason(const char *reason) { (void) reason; }
gnss_fix_status_t gnss_fix_get_status(void) { return (gnss_fix_status_t){ 0 }; }
ntrip_conn_status_t status_ntrip_get(void) { return (ntrip_conn_status_t){ 0 }; }
bool license_has(license_feature_t f) { (void) f; return true; }
gnss_chip_t gnss_detect_effective(gnss_chip_t c) { return c; }
void gnss_unicore_ppp_get(gnss_unicore_ppp_t *out) { memset(out, 0, sizeof(*out)); }

#define GRADI_PER_M (1.0 / M_PER_DEG)

PROVA(media_e_varianza_incrementali_come_calcolo_diretto)
{
    acc_t a = { 0 };
    enum { N = 1000 };
    static double v[N][3];
    double somma[3] = { 0 };
    for (int i = 0; i < N; i++) {
        // valori grandi + piccole variazioni: il caso critico per la precisione
        v[i][0] = 41.123456789 + ((i * 37) % 101 - 50) * 1e-8;
        v[i][1] = 16.987654321 + ((i * 53) % 97 - 48) * 1e-8;
        v[i][2] = 450.0 + ((i * 29) % 89 - 44) * 1e-3;
        acc_add(&a, v[i]);
        for (int k = 0; k < 3; k++) somma[k] += v[i][k];
    }
    VERIFICA_INT(a.n, N);
    for (int k = 0; k < 3; k++) {
        double media = somma[k] / N, sq = 0;
        for (int i = 0; i < N; i++) sq += (v[i][k] - media) * (v[i][k] - media);
        VERIFICA_VICINO(a.mean[k], media, 1e-11);
        VERIFICA_VICINO(a.m2[k], sq, sq * 1e-6 + 1e-18);
    }
}

PROVA(coordinate_impossibili_sempre_scartate)
{
    acc_t a = { 0 };
    const double lat_fuori[3] = { 91.0, 16.0, 400.0 };
    const double lon_fuori[3] = { 41.0, -180.5, 400.0 };
    VERIFICA(acc_is_outlier(&a, lat_fuori, 0.5, 1.0));
    VERIFICA(acc_is_outlier(&a, lon_fuori, 0.5, 1.0));
}

PROVA(primi_dieci_campioni_mai_scartati)
{
    acc_t a = { 0 };
    const double base[3] = { 41.0, 16.5, 450.0 };
    for (int i = 0; i < 9; i++) acc_add(&a, base);
    const double lontano[3] = { 41.01, 16.5, 450.0 }; // ~1 km
    VERIFICA(!acc_is_outlier(&a, lontano, 0.5, 1.0));
    acc_add(&a, base); // 10 campioni
    VERIFICA(acc_is_outlier(&a, lontano, 0.5, 1.0));
}

PROVA(soglie_orizzontale_e_verticale_dopo_dieci_campioni)
{
    acc_t a = { 0 };
    const double base[3] = { 41.0, 16.5, 450.0 };
    for (int i = 0; i < 20; i++) acc_add(&a, base);
    // RTK fisso: 0,5 m orizzontale, 1 m verticale
    double nord_04[3] = { 41.0 + 0.4 * GRADI_PER_M, 16.5, 450.0 };
    double nord_06[3] = { 41.0 + 0.6 * GRADI_PER_M, 16.5, 450.0 };
    double su_09[3] = { 41.0, 16.5, 450.9 };
    double giu_11[3] = { 41.0, 16.5, 448.9 };
    VERIFICA(!acc_is_outlier(&a, nord_04, 0.5, 1.0));
    VERIFICA(acc_is_outlier(&a, nord_06, 0.5, 1.0));
    VERIFICA(!acc_is_outlier(&a, su_09, 0.5, 1.0));
    VERIFICA(acc_is_outlier(&a, giu_11, 0.5, 1.0));
    // float: 5 m / 10 m
    VERIFICA(!acc_is_outlier(&a, nord_06, 5.0, 10.0));
}

PROVA(scarto_est_tiene_conto_della_latitudine)
{
    // A 60 gradi un grado di longitudine vale meta': 0,8 m "in gradi" a
    // 0 gradi sono 0,4 m veri a 60 N -> non scartato con soglia 0,5 m.
    acc_t a = { 0 };
    const double base[3] = { 60.0, 10.0, 100.0 };
    for (int i = 0; i < 20; i++) acc_add(&a, base);
    double est[3] = { 60.0, 10.0 + 0.8 * GRADI_PER_M, 100.0 };
    VERIFICA(!acc_is_outlier(&a, est, 0.5, 1.0));
    double est2[3] = { 60.0, 10.0 + 1.2 * GRADI_PER_M, 100.0 }; // 0,6 m veri
    VERIFICA(acc_is_outlier(&a, est2, 0.5, 1.0));
}

PROVA(dispersione_in_metri)
{
    acc_t a = { 0 };
    double h, v;
    acc_spread_m(&a, &h, &v);
    VERIFICA_VICINO(h, 0, 0);
    VERIFICA_VICINO(v, 0, 0);
    // due campioni a +-0,1 m in nord e +-0,2 m in quota (all'equatore):
    // deviazione standard campionaria = 0,1*sqrt(2) e 0,2*sqrt(2)
    const double p1[3] = { 0.1 * GRADI_PER_M, 0, 10.2 }, p2[3] = { -0.1 * GRADI_PER_M, 0, 9.8 };
    acc_add(&a, p1);
    acc_add(&a, p2);
    acc_spread_m(&a, &h, &v);
    VERIFICA_VICINO(h, 0.1 * sqrt(2.0), 1e-9);
    VERIFICA_VICINO(v, 0.2 * sqrt(2.0), 1e-9);
}

PROVA(salto_has_solo_dopo_cinque_minuti_e_oltre_le_soglie)
{
    memset(&s_has, 0, sizeof(s_has));
    gnss_unicore_ppp_t p = { .valid = true, .lat_deg = 41.0, .lon_deg = 16.5, .h_ell_m = 450.0, .sig_h_m = 0.5f };
    // media pesata in 41.0 / 16.5 / 450
    s_has.sw = 1.0;
    s_has.swx[0] = 41.0; s_has.swx[1] = 16.5; s_has.swx[2] = 450.0;
    s_has.ppp.n = HAS_JUMP_AFTER_N - 1;
    p.lat_deg = 41.0 + 15.0 * GRADI_PER_M; // salto di 15 m
    VERIFICA(!has_is_jump(&p, 1.4f)); // troppo presto
    s_has.ppp.n = HAS_JUMP_AFTER_N;
    VERIFICA(has_is_jump(&p, 1.4f));  // il caso del 07/10: 15 m con 1,4 m dichiarati
    p.lat_deg = 41.0 + 1.9 * GRADI_PER_M; // sotto il minimo di 2 m
    VERIFICA(!has_is_jump(&p, 0.1f));
    p.lat_deg = 41.0 + 4.0 * GRADI_PER_M; // 4 m, ma incertezza dichiarata 1,5 -> limite 4,5 m
    VERIFICA(!has_is_jump(&p, 1.5f));
    p.lat_deg = 41.0;
    p.h_ell_m = 453.5; // 3,5 m in quota, limite max(3*0,5, 3) = 3 m
    VERIFICA(has_is_jump(&p, 0.5f));
}

PROVA(anno_della_compilazione_come_ripiego)
{
    // "Oct  9 2026" (host_rt.c) -> giorno 282 del 2026, circa 2026.77
    VERIFICA_VICINO(build_decimal_year(), 2026.77, 0.01);
}

int main(void)
{
    ESEGUI(media_e_varianza_incrementali_come_calcolo_diretto);
    ESEGUI(coordinate_impossibili_sempre_scartate);
    ESEGUI(primi_dieci_campioni_mai_scartati);
    ESEGUI(soglie_orizzontale_e_verticale_dopo_dieci_campioni);
    ESEGUI(scarto_est_tiene_conto_della_latitudine);
    ESEGUI(dispersione_in_metri);
    ESEGUI(salto_has_solo_dopo_cinque_minuti_e_oltre_le_soglie);
    ESEGUI(anno_della_compilazione_come_ripiego);
    return prove_fine();
}
