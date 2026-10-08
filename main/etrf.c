#include "etrf.h"

#include <math.h>
#include <stdbool.h>

// Ellissoide GRS80 (ITRF ed ETRF).
#define GRS80_A   6378137.0
#define GRS80_F   (1.0 / 298.257222101)
#define GRS80_E2  (GRS80_F * (2.0 - GRS80_F))

#define MAS_TO_RAD (M_PI / 180.0 / 3600.0 / 1000.0)

// EPSG 10586, epoca 2015.0: traslazioni in m, rotazioni in milliarcosecondi,
// scala in parti per miliardo; poi le variazioni annue.
static const double P_T[3]  = { 0.0538, 0.0518, -0.0822 };
static const double P_R[3]  = { 2.106, 12.74, -20.592 };
static const double P_D     = 2.25;
static const double DP_T[3] = { 0.0001, 0.0, -0.0017 };
static const double DP_R[3] = { 0.081, 0.49, -0.792 };
static const double DP_D    = 0.11;

static void geo_to_ecef(double lat_deg, double lon_deg, double h, double xyz[3])
{
    double la = lat_deg * M_PI / 180.0, lo = lon_deg * M_PI / 180.0;
    double s = sin(la), c = cos(la);
    double n = GRS80_A / sqrt(1.0 - GRS80_E2 * s * s);
    xyz[0] = (n + h) * c * cos(lo);
    xyz[1] = (n + h) * c * sin(lo);
    xyz[2] = (n * (1.0 - GRS80_E2) + h) * s;
}

static void ecef_to_geo(const double xyz[3], double *lat_deg, double *lon_deg, double *h)
{
    double p = hypot(xyz[0], xyz[1]);
    double lat = atan2(xyz[2], p * (1.0 - GRS80_E2));
    double hh = 0;
    for (int i = 0; i < 10; i++) {
        double s = sin(lat);
        double n = GRS80_A / sqrt(1.0 - GRS80_E2 * s * s);
        hh = p / cos(lat) - n;
        lat = atan2(xyz[2], p * (1.0 - GRS80_E2 * n / (n + hh)));
    }
    *lat_deg = lat * 180.0 / M_PI;
    *lon_deg = atan2(xyz[1], xyz[0]) * 180.0 / M_PI;
    *h = hh;
}

void etrf_itrf2020_to_etrf2000(double *lat_deg, double *lon_deg, double *h_m, double epoch)
{
    double dt = epoch - 2015.0;
    double t[3], r[3];
    for (int i = 0; i < 3; i++) {
        t[i] = P_T[i] + DP_T[i] * dt;
        r[i] = (P_R[i] + DP_R[i] * dt) * MAS_TO_RAD;
    }
    double d = (P_D + DP_D * dt) * 1e-9;

    double x[3], y[3];
    geo_to_ecef(*lat_deg, *lon_deg, *h_m, x);
    // Position vector: X' = T + (1 + D) X + R x X
    y[0] = t[0] + (1.0 + d) * x[0] - r[2] * x[1] + r[1] * x[2];
    y[1] = t[1] + r[2] * x[0] + (1.0 + d) * x[1] - r[0] * x[2];
    y[2] = t[2] - r[1] * x[0] + r[0] * x[1] + (1.0 + d) * x[2];
    ecef_to_geo(y, lat_deg, lon_deg, h_m);
}

double etrf_decimal_year(time_t t)
{
    struct tm tm;
    gmtime_r(&t, &tm);
    int year = tm.tm_year + 1900;
    bool leap = (year % 4 == 0 && year % 100 != 0) || year % 400 == 0;
    double days = leap ? 366.0 : 365.0;
    double sec = tm.tm_yday * 86400.0 + tm.tm_hour * 3600.0 + tm.tm_min * 60.0 + tm.tm_sec;
    return year + sec / (days * 86400.0);
}
