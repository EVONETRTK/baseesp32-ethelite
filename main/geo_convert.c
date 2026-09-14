#include "geo_convert.h"

#include <math.h>

// Parametri dell'ellissoide WGS84 (stessi valori usati da RTCM3/GPS).
#define WGS84_A  6378137.0
#define WGS84_F  (1.0 / 298.257223563)
#define WGS84_E2 (WGS84_F * (2.0 - WGS84_F))

void geo_llh_to_ecef(double lat_deg, double lon_deg, double height_m,
                      double *x, double *y, double *z)
{
    double lat = lat_deg * M_PI / 180.0;
    double lon = lon_deg * M_PI / 180.0;
    double sin_lat = sin(lat);
    double cos_lat = cos(lat);
    double n = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);

    *x = (n + height_m) * cos_lat * cos(lon);
    *y = (n + height_m) * cos_lat * sin(lon);
    *z = (n * (1.0 - WGS84_E2) + height_m) * sin_lat;
}

// Metodo iterativo (Bowring semplificato): converge a livello
// submillimetrico in 5 iterazioni per qualunque punto vicino alla
// superficie terrestre, che e' l'unico caso d'uso qui (posizione di
// un'antenna GNSS).
void geo_ecef_to_llh(double x, double y, double z,
                      double *lat_deg, double *lon_deg, double *height_m)
{
    double lon = atan2(y, x);
    double p = sqrt(x * x + y * y);
    double lat = atan2(z, p * (1.0 - WGS84_E2));

    double n = WGS84_A;
    double h = 0;
    for (int i = 0; i < 5; i++) {
        double sin_lat = sin(lat);
        n = WGS84_A / sqrt(1.0 - WGS84_E2 * sin_lat * sin_lat);
        h = p / cos(lat) - n;
        lat = atan2(z, p * (1.0 - WGS84_E2 * n / (n + h)));
    }

    *lat_deg = lat * 180.0 / M_PI;
    *lon_deg = lon * 180.0 / M_PI;
    *height_m = h;
}
