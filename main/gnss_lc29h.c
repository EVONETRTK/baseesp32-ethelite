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

    // Il comando documentato (PAIR432) del LC29H alza/abbassa la
    // risoluzione RTCM per TUTTE le costellazioni insieme - a differenza
    // di u-blox/Unicore, questo modulo non ha un comando per scegliere
    // MSM4/MSM7 (o spegnere del tutto) una costellazione alla volta, ne'
    // per spegnere singolarmente 1007/1008/1019/1020/1230. Il livello
    // scelto qui e' MSM7 se e' spuntato ALMENO UNO dei messaggi MSM7
    // (1077/1087/1097/1127), altrimenti MSM4 se e' spuntato almeno un MSM4
    // - limite del set di comandi del modulo, non del firmware: se anche
    // una sola costellazione chiede MSM7, tutte le altre attive lo
    // riceveranno comunque a MSM7.
    bool any_msm7 = s.rtcm_1077_enable || s.rtcm_1087_enable || s.rtcm_1097_enable || s.rtcm_1127_enable;
    bool any_msm4 = s.rtcm_1074_enable || s.rtcm_1084_enable || s.rtcm_1094_enable || s.rtcm_1124_enable;
    if (!any_msm7 && !any_msm4) {
        ESP_LOGW(TAG, "Nessun messaggio MSM4/MSM7 selezionato nelle impostazioni: il LC29H non ha un comando "
                      "documentato per spegnere del tutto l'uscita RTCM in modalita' base, restera' al livello "
                      "di default del modulo (MSM4)");
    }
    if (s.rtcm_1007_enable || s.rtcm_1008_enable || s.rtcm_1019_enable || s.rtcm_1020_enable) {
        ESP_LOGW(TAG, "1007/1008/1019/1020 richiesti nelle impostazioni ma non supportati dal LC29H in uscita: ignorati");
    }
    if (!s.rtcm_1230_enable) {
        ESP_LOGW(TAG, "1230 disattivato nelle impostazioni ma il LC29H non ha un comando documentato per "
                      "sopprimerlo singolarmente: potrebbe restare comunque incluso dal default del modulo");
    }
    bool use_msm7 = any_msm7;

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
        ESP_LOGI(TAG, "Configuro ricevitore Quectel LC29H come base RTK (posizione fissa manuale + RTCM3 %s)",
                 use_msm7 ? "MSM7" : "MSM4");
        snprintf(svin_cmd, sizeof(svin_cmd), "PQTMCFGSVIN,W,2,0,0,%.4f,%.4f,%.4f", x, y, z);
    } else {
        ESP_LOGI(TAG, "Configuro ricevitore Quectel LC29H come base RTK (survey-in + RTCM3 %s)",
                 use_msm7 ? "MSM7" : "MSM4");
        snprintf(svin_cmd, sizeof(svin_cmd), "PQTMCFGSVIN,W,1,60,2.5,0,0,0");
    }

    // PQTMCFGRCVRMODE,W,2: modalita' base - abilita da sola RTCM MSM4+1005
    // e disattiva l'uscita NMEA, per documentazione ufficiale.
    // PAIR432: 0 = resta a MSM4 (default della modalita' base), 1 = alza a
    // MSM7 - deciso sopra da requested/use_msm7 invece che sempre fisso a 1.
    // PAIR434: forza/sopprime esplicitamente l'uscita del messaggio 1005
    // (posizione antenna) secondo rtcm_1005_enable - non e' garantito che
    // il modulo onori una richiesta di soppressione, non documentato.
    // PQTMSAVEPAR: salva SVIN e RCVRMODE (richiesto da entrambi per avere
    // effetto, vedi ATTENZIONE in gnss_lc29h.h sul riavvio del modulo).
    char pair432_cmd[16];
    snprintf(pair432_cmd, sizeof(pair432_cmd), "PAIR432,%d", use_msm7 ? 1 : 0);
    char pair434_cmd[16];
    snprintf(pair434_cmd, sizeof(pair434_cmd), "PAIR434,%d", s.rtcm_1005_enable ? 1 : 0);

    const char *const cmds[] = {
        svin_cmd,
        "PQTMCFGRCVRMODE,W,2",
        pair432_cmd,
        pair434_cmd,
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
