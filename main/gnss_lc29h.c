#include "gnss_lc29h.h"
#include "settings.h"
#include "geo_convert.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gnss_lc29h";

// Checksum NMEA standard (XOR di tutti i byte tra '$' e '*'), usato sia
// dalle sentenze NMEA sia dai comandi proprietari $PQTM.../$PAIR... di
// questo modulo - calcolato a runtime invece di scrivere valori fissi nel
// codice, per restare corretto anche se in futuro cambiano i parametri dei
// comandi qui sotto.
static uint8_t nmea_checksum(const char *body)
{
    uint8_t cksum = 0;
    for (const char *p = body; *p; p++) {
        cksum ^= (uint8_t) *p;
    }
    return cksum;
}

// body = il comando SENZA '$' iniziale ne' '*checksum' finale, es.
// "PQTMCFGRCVRMODE,W,2".
static esp_err_t send_cmd(uart_port_t uart_num, const char *body)
{
    char line[128];
    int n = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, nmea_checksum(body));
    ESP_LOGI(TAG, "-> %s", body);
    int written = uart_write_bytes(uart_num, line, n);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == n) ? ESP_OK : ESP_FAIL;
}

esp_err_t gnss_lc29h_configure_base(uart_port_t uart_num)
{
    app_settings_t s = settings_get();

    // PQTMCFGSVIN, campo <Mode>: 1 = survey-in (media pesata delle
    // posizioni per <MinDur> secondi, precisione richiesta <3D_AccLimit>
    // metri, i tre campi ECEF restano ignorati), 2 = "fixed" con posizione
    // nota passata direttamente nei campi ECEF X/Y/Z (documentazione
    // ufficiale Quectel, par. 2.3.8) - qui convertita da lat/lon/quota
    // WGS84 (piu' leggibili, quello che restituisce un servizio PPP) con
    // geo_convert.h.
    char svin_cmd[128];
    if (s.base_position_mode == BASE_POSITION_MANUAL) {
        double x, y, z;
        geo_llh_to_ecef(s.base_fixed_lat_deg, s.base_fixed_lon_deg, s.base_fixed_height_m, &x, &y, &z);
        ESP_LOGI(TAG, "Configuro ricevitore Quectel LC29H come base RTK (posizione fissa manuale + RTCM3 MSM7)");
        snprintf(svin_cmd, sizeof(svin_cmd), "PQTMCFGSVIN,W,2,0,0,%.4f,%.4f,%.4f", x, y, z);
    } else {
        ESP_LOGI(TAG, "Configuro ricevitore Quectel LC29H come base RTK (survey-in + RTCM3 MSM7)");
        snprintf(svin_cmd, sizeof(svin_cmd), "PQTMCFGSVIN,W,1,60,2.5,0,0,0");
    }

    // PQTMCFGRCVRMODE,W,2: modalita' base - abilita da sola RTCM MSM4+1005
    // e disattiva l'uscita NMEA, per documentazione ufficiale.
    // PAIR432,1: alza l'uscita RTCM da MSM4 (default della modalita' base)
    // a MSM7, piu' preciso.
    // PAIR434,1: forza esplicitamente l'uscita del messaggio 1005
    // (posizione antenna) - ridondante col default della modalita' base,
    // ma innocuo ed esplicito.
    // PQTMSAVEPAR: salva SVIN e RCVRMODE (richiesto da entrambi per avere
    // effetto, vedi ATTENZIONE in gnss_lc29h.h sul riavvio del modulo).
    const char *const cmds[] = {
        svin_cmd,
        "PQTMCFGRCVRMODE,W,2",
        "PAIR432,1",
        "PAIR434,1",
        "PQTMSAVEPAR",
    };

    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t e = send_cmd(uart_num, cmds[i]);
        if (e != ESP_OK) {
            err = e;
        }
    }
    return err;
}

esp_err_t gnss_lc29h_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Quectel LC29H come rover (riceve RTCM3, emette $GxGGA)");

    // PQTMCFGRCVRMODE,W,1: modalita' rover - ripristina l'uscita NMEA
    // standard (incluso GGA) per documentazione ufficiale. L'ingresso RTCM3
    // (ricevuto sulla stessa UART da ntrip_rover_client.c) e' gia'
    // supportato in automatico su questa variante, nessun comando dedicato
    // necessario per abilitarlo.
    static const char *const cmds[] = {
        "PQTMCFGRCVRMODE,W,1",
        "PQTMSAVEPAR",
    };

    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t e = send_cmd(uart_num, cmds[i]);
        if (e != ESP_OK) {
            err = e;
        }
    }
    return err;
}
