#include "gnss_unicore.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gnss_unicore";

static esp_err_t send_cmd(uart_port_t uart_num, const char *cmd)
{
    ESP_LOGI(TAG, "-> %s", cmd);
    int len = (int) strlen(cmd);
    int written = uart_write_bytes(uart_num, cmd, len);
    uart_write_bytes(uart_num, "\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}

// Numero messaggio RTCM3 MSM4/MSM7 per costellazione - stessa convenzione
// di nome comando "RTCM<numero> 1" gia' usata per i messaggi MSM4 in questo
// file, estesa qui ai numeri MSM7 seguendo lo stesso schema standard
// RTCM3 (non e' stata verificata riga per riga sul manuale comandi Unicore
// UM98x, ma segue lo schema di denominazione gia' confermato funzionante
// per gli altri numeri messaggio in questa stessa funzione).
typedef struct {
    int msm4_type;
    int msm7_type;
} unicore_msm_types_t;

static const unicore_msm_types_t UNICORE_MSM_GPS     = { 1074, 1077 };
static const unicore_msm_types_t UNICORE_MSM_GLONASS = { 1084, 1087 };
static const unicore_msm_types_t UNICORE_MSM_GALILEO = { 1094, 1097 };
static const unicore_msm_types_t UNICORE_MSM_BEIDOU  = { 1124, 1127 };

static esp_err_t send_msm_level(uart_port_t uart_num, const unicore_msm_types_t *types, rtcm_msm_level_t level)
{
    if (level == RTCM_MSM_OFF) {
        return ESP_OK;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "RTCM%d 1", level == RTCM_MSM4 ? types->msm4_type : types->msm7_type);
    return send_cmd(uart_num, cmd);
}

esp_err_t gnss_unicore_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore come base RTK (auto-survey + RTCM3)");

    app_settings_t s = settings_get();

    // UNLOG azzera tutti i log/messaggi attivi: non serve disattivare
    // esplicitamente un livello MSM non scelto, parte gia' da zero.
    esp_err_t err = send_cmd(uart_num, "UNLOG");
    esp_err_t e = send_cmd(uart_num, "MODE BASE TIME 60 2.5");
    if (e != ESP_OK) err = e;
    if (s.rtcm_1005_enable) {
        e = send_cmd(uart_num, "RTCM1005 1");
        if (e != ESP_OK) err = e;
    }
    e = send_cmd(uart_num, "RTCM1033 1"); // descrittore antenna, sempre utile, non fa parte della selezione
    if (e != ESP_OK) err = e;
    if (s.rtcm_1230_enable) {
        e = send_cmd(uart_num, "RTCM1230 1");
        if (e != ESP_OK) err = e;
    }
    e = send_msm_level(uart_num, &UNICORE_MSM_GPS, s.rtcm_gps_msm);
    if (e != ESP_OK) err = e;
    e = send_msm_level(uart_num, &UNICORE_MSM_GLONASS, s.rtcm_glonass_msm);
    if (e != ESP_OK) err = e;
    e = send_msm_level(uart_num, &UNICORE_MSM_GALILEO, s.rtcm_galileo_msm);
    if (e != ESP_OK) err = e;
    e = send_msm_level(uart_num, &UNICORE_MSM_BEIDOU, s.rtcm_beidou_msm);
    if (e != ESP_OK) err = e;
    e = send_cmd(uart_num, "SAVECONFIG");
    if (e != ESP_OK) err = e;

    return err;
}

esp_err_t gnss_unicore_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore come rover (riceve RTCM3, emette $GxGGA)");

    // Modalita' rover standard Unicore UM98x, con uscita NMEA GGA a 1Hz
    // (usata da ntrip_rover_client.c per inoltrare la posizione al caster).
    static const char *const cmds[] = {
        "UNLOG",
        "MODE ROVER",
        "GPGGA 1",
        "SAVECONFIG",
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
