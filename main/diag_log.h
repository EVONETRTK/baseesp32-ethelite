#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "esp_http_server.h"

void diag_log_start(void);

// Log salvati sulla microSD (uno per avvio, ultimi 5: boot_0..4.log).
typedef struct {
    int index;
    uint32_t size;
    bool current; // quello dell'avvio in corso
} diag_log_file_t;

// Riempie out con i file presenti; -1 se la SD non e' disponibile.
int diag_log_list(diag_log_file_t *out, int max);

// Manda il file boot_<index>.log come allegato di testo (senza codici colore).
esp_err_t diag_log_send_http(httpd_req_t *req, int index);
