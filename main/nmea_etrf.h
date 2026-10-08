#pragma once

#include <stdbool.h>
#include <stddef.h>

// Rover con Galileo HAS come riserva: quando il ricevitore non lavora in RTK
// (niente correzioni dalla base) le sue coordinate sono ITRF, mentre in RTK
// sono nel sistema della base (ETRF2000 per la rete EVONETRTK). Per evitare
// un salto di ~1 m ad AgOpenGPS ogni volta che si passa da uno all'altro, le
// frasi GGA e RMC fuori dall'RTK vengono riscritte in ETRF2000 (etrf.h).

// Riscrive sul posto latitudine, longitudine (e in GGA la quota) di una
// riga NMEA GGA o RMC senza "\r\n", ricalcolando il checksum. Mantiene il
// numero di decimali dell'originale. cap = dimensione del buffer.
// h_ell_m: quota ellissoidica da usare per RMC (che non la porta).
// false (riga invariata) se non e' GGA/RMC o non ha una posizione.
bool nmea_etrf_convert(char *line, size_t cap, double epoch, double h_ell_m);

// Qualita' GGA (campo 6) e quota ellissoidica (quota + separazione del
// geoide); -1 / false se mancano.
int nmea_gga_quality(const char *line);
bool nmea_gga_ellipsoidal_height(const char *line, double *h_m);

// Come nmea_etrf_convert, ma mette nella riga la posizione data (ITRF2020,
// es. la soluzione Galileo HAS del log PPPNAVA) invece di quella della riga;
// in GGA anche qualita' e eta' delle correzioni (quality/age < 0: invariati).
bool nmea_etrf_set_position(char *line, size_t cap, double epoch, double lat_deg, double lon_deg,
                            double h_ell_m, int quality, float age_s);

// Latitudine e longitudine (gradi) di una riga GGA; false se mancano.
bool nmea_gga_latlon(const char *line, double *lat_deg, double *lon_deg);
