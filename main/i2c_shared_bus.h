#pragma once

#include "driver/i2c_master.h"

// Bus I2C condiviso tra i dispositivi sugli stessi pin (display OLED,
// ricevitore GNSS u-blox via I2C su un HAT). Il driver i2c_master di ESP-IDF
// serializza gia' le transazioni sullo stesso bus: basta che tutti usino lo
// stesso handle invece di crearne uno a testa (un secondo
// i2c_new_master_bus() sugli stessi pin fallirebbe).
//
// Ritorna il bus esistente se gia' creato con gli stessi pin, lo crea
// altrimenti. NULL se i pin non sono validi, se la creazione fallisce o se
// il bus esiste gia' su pin diversi. Da chiamare dal task main all'avvio.
i2c_master_bus_handle_t i2c_shared_bus_get(int sda_pin, int scl_pin);
