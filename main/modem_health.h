// Salute del modem SIM7600: temperatura, tensione, qualita' del 4G e
// versione del firmware, lette dalle risposte AT. Solo lettura del testo
// (nessuna dipendenza da ESP-IDF): provata sul PC (tests/host/test_modem_health.c).
#pragma once

#include <stdbool.h>
#include <stddef.h>

typedef struct {
    bool temp_ok;
    int temp_c;            // AT+CPMUTEMP
    bool volt_ok;
    int volt_mv;           // AT+CBC
    bool lte_ok;           // AT+CPSI? con il modem in LTE
    char mode[16];         // "LTE", "WCDMA", "GSM", "NO SERVICE"...
    char mcc_mnc[8];       // "222-88"
    int band;              // EUTRAN-BAND20 -> 20 (0 = sconosciuta)
    unsigned long cell_id;
    float rsrp_dbm;        // potenza del segnale di riferimento (-140..-44)
    float rsrq_db;         // qualita' (-20..-3)
    float rssi_dbm;
    int sinr_db;           // rapporto segnale/rumore (RSSNR)
    char fw[40];           // AT+CGMR
} modem_health_t;

bool modem_health_parse_temp(const char *resp, int *temp_c);
bool modem_health_parse_cbc(const char *resp, int *volt_mv);
// Riempie mode e, se in LTE, i campi lte_*; false se la risposta non c'e'.
bool modem_health_parse_cpsi(const char *resp, modem_health_t *h);
bool modem_health_parse_cgmr(const char *resp, char *out, size_t out_size);
