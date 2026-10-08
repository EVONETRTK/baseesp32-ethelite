#pragma once

#include <time.h>

// Conversione di coordinate da ITRF2020 (all'epoca della misura: quello che
// danno Galileo HAS e il PPP) a ETRF2000, il sistema delle reti italiane
// (RDN2008) scelto per tutte le basi EVONETRTK.
//
// Parametri EPSG 10586 "ITRF2020 to ETRF2000 (1)" (EUREF Technical Note 1,
// marzo 2024), metodo "Time-dependent Position Vector tfm (geocentric)",
// epoca di riferimento 2015.0. In Italia lo spostamento e' di circa 1 m
// verso sud-ovest nel 2026 (moto della placca euroasiatica dal 1989).
// Non tiene conto dei moti locali dentro la placca (pochi mm/anno, in
// Puglia e Sicilia qualche cm dal 2008): sotto la precisione di HAS.
//
// lat/lon in gradi, h quota ellissoidica in metri (GRS80), epoch in anni
// decimali (es. 2026.77). Converte sul posto.
void etrf_itrf2020_to_etrf2000(double *lat_deg, double *lon_deg, double *h_m, double epoch);

// Anno decimale di un istante (es. 2026.77 per l'8 ottobre 2026).
double etrf_decimal_year(time_t t);
