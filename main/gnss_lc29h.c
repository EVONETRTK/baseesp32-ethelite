#include "gnss_lc29h.h"

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
    ESP_LOGI(TAG, "Configuro ricevitore Quectel LC29H come base RTK (survey-in + RTCM3 MSM7)");

    // PQTMCFGSVIN: modalita' survey-in (1), durata minima 60s, precisione
    // richiesta 2,5m (stessa convenzione gia' usata per Unicore sopra) - i
    // tre campi ECEF X/Y/Z restano a 0 (ignorati in survey-in, servono solo
    // in modalita' "fixed" con coordinate note a mano).
    // PQTMCFGRCVRMODE,W,2: modalita' base - abilita da sola RTCM MSM4+1005
    // e disattiva l'uscita NMEA, per documentazione ufficiale.
    // PAIR432,1: alza l'uscita RTCM da MSM4 (default della modalita' base)
    // a MSM7, piu' preciso.
    // PAIR434,1: forza esplicitamente l'uscita del messaggio 1005
    // (posizione antenna) - ridondante col default della modalita' base,
    // ma innocuo ed esplicito.
    // PQTMSAVEPAR: salva SVIN e RCVRMODE (richiesto da entrambi per avere
    // effetto, vedi ATTENZIONE in gnss_lc29h.h sul riavvio del modulo).
    static const char *const cmds[] = {
        "PQTMCFGSVIN,W,1,60,2.5,0,0,0",
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
