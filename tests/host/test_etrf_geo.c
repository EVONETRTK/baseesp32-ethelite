// Prove di main/etrf.c (ITRF2020 -> ETRF2000) e main/geo_convert.c
// (geodetiche WGS84 <-> ECEF).
//
// Valori attesi ETRF calcolati in Python (tests/host/strumenti/
// riferimenti.py, sezione "ETRF") con i parametri pubblicati da EUREF,
// Technical Note 1 (2024) / EPSG 10586, epoca di riferimento 2015.0:
//   T  = ( 53.8,  51.8, -82.2) mm       dT = (0.1, 0.0, -1.7) mm/anno
//   D  = 2.25 ppb                        dD = 0.11 ppb/anno
//   R  = (2.106, 12.740, -20.592) mas    dR = (0.081, 0.490, -0.792) mas/anno
//   X_E = X_I + T + D X_I + [[0,-R3,R2],[R3,0,-R1],[-R2,R1,0]] X_I
// e geodetiche <-> ECEF con il metodo chiuso di Heikkinen (non l'iterazione
// del firmware), ellissoide GRS80.
//   41.0 N 16.5 E h 450 epoca 2026.5 -> 40.9999942510 16.4999902045 450.0048
//       (dN -0.6384 dE -0.8241 m, 1,04 m)
//   45.5 N  9.2 E h 150 epoca 2026.0 ->  45.4999941244  9.1999907574 149.9920
//       (dN -0.6530 dE -0.7224 m, 0,97 m)
//   38.1 N 13.4 E h  50 epoca 2030.0 ->  38.0999936467 13.3999897022  50.0083
#include "prove.h"
#include "../../main/etrf.c"
#include "../../main/geo_convert.c"

#define M_PER_DEG_LAT 111132.0

typedef struct {
    double lat, lon, h, ep;
    double lat2, lon2, h2;
} caso_etrf_t;

static const caso_etrf_t CASI[] = {
    { 41.0, 16.5, 450.0, 2026.5, 40.9999942510, 16.4999902045, 450.0048 },
    { 45.5, 9.2, 150.0, 2026.0, 45.4999941244, 9.1999907574, 149.9920 },
    { 38.1, 13.4, 50.0, 2030.0, 38.0999936467, 13.3999897022, 50.0083 },
};

PROVA(etrf_come_calcolo_indipendente_python)
{
    for (size_t i = 0; i < sizeof(CASI) / sizeof(CASI[0]); i++) {
        const caso_etrf_t *c = &CASI[i];
        double lat = c->lat, lon = c->lon, h = c->h;
        etrf_itrf2020_to_etrf2000(&lat, &lon, &h, c->ep);
        // 1e-9 gradi = 0,1 mm; quota stampata da Python con 4 decimali
        VERIFICA_VICINO(lat, c->lat2, 2e-10 + 1e-10);
        VERIFICA_VICINO(lon, c->lon2, 2e-10 + 1e-10);
        VERIFICA_VICINO(h, c->h2, 0.0001);
    }
}

PROVA(etrf_in_italia_nel_2026_circa_097_m_verso_sud_ovest)
{
    // Punti dalle Alpi a Lampedusa: spostamento orizzontale tra 0,9 e 1,15 m,
    // sempre verso sud-ovest, quota quasi invariata.
    const double punti[][2] = { { 45.7, 7.3 }, { 45.6, 13.8 }, { 41.9, 12.5 }, { 40.8, 16.6 }, { 37.5, 15.1 }, { 35.5, 12.6 } };
    for (size_t i = 0; i < sizeof(punti) / sizeof(punti[0]); i++) {
        double lat = punti[i][0], lon = punti[i][1], h = 300.0;
        etrf_itrf2020_to_etrf2000(&lat, &lon, &h, 2026.0);
        double dn = (lat - punti[i][0]) * M_PER_DEG_LAT;
        double de = (lon - punti[i][1]) * M_PER_DEG_LAT * cos(punti[i][0] * M_PI / 180.0);
        double oriz = hypot(dn, de);
        VERIFICA(dn < 0);
        VERIFICA(de < 0);
        VERIFICA(oriz > 0.9 && oriz < 1.15);
        VERIFICA(fabs(h - 300.0) < 0.05);
    }
}

PROVA(etrf_spostamento_cresce_con_gli_anni)
{
    // ~2,5 cm/anno: tra 2026 e 2030 lo spostamento cresce di circa 10 cm
    double a[3] = { 42.0, 13.0, 200.0 }, b[3] = { 42.0, 13.0, 200.0 };
    etrf_itrf2020_to_etrf2000(&a[0], &a[1], &a[2], 2026.0);
    etrf_itrf2020_to_etrf2000(&b[0], &b[1], &b[2], 2030.0);
    double da = hypot((a[0] - 42.0) * M_PER_DEG_LAT, (a[1] - 13.0) * M_PER_DEG_LAT * cos(42.0 * M_PI / 180));
    double db = hypot((b[0] - 42.0) * M_PER_DEG_LAT, (b[1] - 13.0) * M_PER_DEG_LAT * cos(42.0 * M_PI / 180));
    VERIFICA_VICINO(db - da, 0.10, 0.02);
}

PROVA(etrf_geodetiche_ecef_interne_andata_e_ritorno)
{
    for (double lat = -85; lat <= 85; lat += 17) {
        for (double lon = -175; lon <= 175; lon += 35) {
            for (double h = -100; h <= 9000; h += 3000) {
                double xyz[3], la, lo, hh;
                geo_to_ecef(lat, lon, h, xyz);
                ecef_to_geo(xyz, &la, &lo, &hh);
                VERIFICA_VICINO(la, lat, 1e-9);
                VERIFICA_VICINO(lo, lon, 1e-9);
                VERIFICA_VICINO(hh, h, 0.001);
            }
        }
    }
}

PROVA(ecef_di_punti_noti_wgs84)
{
    double x, y, z;
    geo_llh_to_ecef(0, 0, 0, &x, &y, &z);
    VERIFICA_VICINO(x, 6378137.0, 1e-6);
    VERIFICA_VICINO(y, 0, 1e-6);
    VERIFICA_VICINO(z, 0, 1e-6);
    geo_llh_to_ecef(0, 90, 100, &x, &y, &z);
    VERIFICA_VICINO(x, 0, 1e-6);
    VERIFICA_VICINO(y, 6378237.0, 1e-6);
    geo_llh_to_ecef(90, 0, 0, &x, &y, &z);
    VERIFICA_VICINO(z, 6356752.314245, 1e-5); // semiasse minore WGS84
    // Python: 41 N 16.5 E 450 m -> 4622403.4332 1369218.2761 4162718.4272
    geo_llh_to_ecef(41.0, 16.5, 450.0, &x, &y, &z);
    VERIFICA_VICINO(x, 4622403.4332, 0.0001);
    VERIFICA_VICINO(y, 1369218.2761, 0.0001);
    VERIFICA_VICINO(z, 4162718.4272, 0.0001);
}

PROVA(geo_ecef_andata_e_ritorno_sotto_il_millimetro)
{
    double peggiore_h = 0, peggiore_oriz = 0;
    for (double lat = -89.9; lat <= 89.9; lat += 4.99) {
        for (double lon = -179.9; lon <= 180; lon += 12.7) {
            for (double h = -400; h <= 9000; h += 1175) {
                double x, y, z, la, lo, hh;
                geo_llh_to_ecef(lat, lon, h, &x, &y, &z);
                geo_ecef_to_llh(x, y, z, &la, &lo, &hh);
                double dn = (la - lat) * M_PER_DEG_LAT;
                double de = (lo - lon) * M_PER_DEG_LAT * cos(lat * M_PI / 180.0);
                if (fabs(hh - h) > peggiore_h) peggiore_h = fabs(hh - h);
                if (hypot(dn, de) > peggiore_oriz) peggiore_oriz = hypot(dn, de);
            }
        }
    }
    VERIFICA(peggiore_h < 0.001);
    VERIFICA(peggiore_oriz < 0.001);
}

PROVA(geo_ecef_ritorno_vicino_al_polo)
{
    double x, y, z, la, lo, hh;
    geo_llh_to_ecef(89.9999, 45.0, 120.0, &x, &y, &z);
    geo_ecef_to_llh(x, y, z, &la, &lo, &hh);
    VERIFICA_VICINO(la, 89.9999, 1e-9);
    VERIFICA_VICINO(lo, 45.0, 1e-6);
    VERIFICA_VICINO(hh, 120.0, 0.001);
}

int main(void)
{
    ESEGUI(etrf_come_calcolo_indipendente_python);
    ESEGUI(etrf_in_italia_nel_2026_circa_097_m_verso_sud_ovest);
    ESEGUI(etrf_spostamento_cresce_con_gli_anni);
    ESEGUI(etrf_geodetiche_ecef_interne_andata_e_ritorno);
    ESEGUI(ecef_di_punti_noti_wgs84);
    ESEGUI(geo_ecef_andata_e_ritorno_sotto_il_millimetro);
    ESEGUI(geo_ecef_ritorno_vicino_al_polo);
    return prove_fine();
}
