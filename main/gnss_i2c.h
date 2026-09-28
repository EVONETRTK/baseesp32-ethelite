#pragma once

#include <stdbool.h>

// Ricevitore u-blox collegato via I2C (interfaccia "DDC" u-blox, indirizzo
// 0x42) invece che via seriale - es. l'HAT Syneda uRTK6.0 (ZED-F9P), che sul
// connettore a 40 pin porta solo alimentazione e I2C (le seriali sono su
// connettori laterali). Sulla T-ETH-Elite i pin 3/5 del connettore sono
// GPIO17/18, lo stesso bus del display OLED (vedi i2c_shared_bus.h).

// Diagnostica: controlla se all'indirizzo 0x42 risponde un dispositivo e,
// se si', quanti byte ha pronti da leggere. Solo log, nessuna configurazione.
bool gnss_i2c_probe(int sda_pin, int scl_pin);
