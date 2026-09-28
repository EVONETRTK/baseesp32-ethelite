#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "settings.h"

// Canale dati verso il ricevitore GNSS: seriale (UART) oppure I2C u-blox
// (settings.gnss_i2c, vedi gnss_i2c.h). Tutto il firmware legge e scrive
// verso il ricevitore solo da qui, cosi' il resto (base, rover, driver dei
// chip) non dipende da come e' collegato.

// Da chiamare una volta all'avvio, prima di qualunque lettura/scrittura.
void gnss_io_init(const app_settings_t *settings);

bool gnss_io_is_i2c(void);

// Come uart_read_bytes(): attende fino a timeout, ritorna i byte letti
// (0 se nessuno, -1 su errore).
int gnss_io_read(uint8_t *buf, size_t len, TickType_t timeout);

// Come uart_write_bytes(): ritorna i byte accettati (-1 su errore).
int gnss_io_write(const void *data, size_t len);
