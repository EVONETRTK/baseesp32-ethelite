#pragma once

#include "driver/uart.h"
#include "esp_err.h"
#include "settings.h"

// Configura il ricevitore GNSS collegato nella modalita' scelta (base o
// rover), con la sequenza di comandi giusta per il chip selezionato
// (vedi settings.h).
esp_err_t gnss_driver_configure(uart_port_t uart_num, gnss_chip_t chip, device_mode_t mode);
