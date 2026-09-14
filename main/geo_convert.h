#pragma once

// Conversioni tra coordinate geografiche WGS84 (latitudine/longitudine/
// quota ellissoidica, quelle leggibili da una persona e quelle restituite
// dai servizi di post-processing PPP come CSRS-PPP/OPUS) e coordinate
// cartesiane ECEF (quelle usate internamente dal protocollo RTCM3 - vedi
// rtcm3_1005.h - e dal comando $PQTMCFGSVIN del modulo LC29H in modalita'
// "fixed"). Formule standard, indipendenti dal chip GNSS.

// lat_deg/lon_deg in gradi decimali, height_m = quota ellissoidica WGS84
// (NON sul livello del mare) in metri. Scrive il risultato in x/y/z (metri).
void geo_llh_to_ecef(double lat_deg, double lon_deg, double height_m,
                      double *x, double *y, double *z);

// Inversa della funzione sopra (metodo iterativo, converge in poche
// iterazioni a una precisione submillimetrica per qualunque punto sulla
// superficie terrestre).
void geo_ecef_to_llh(double x, double y, double z,
                      double *lat_deg, double *lon_deg, double *height_m);
