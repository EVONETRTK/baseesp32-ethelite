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

// Manda "RTCM<numero> 1" se enabled, altrimenti non fa nulla (UNLOG in
// gnss_unicore_configure_base() azzera gia' tutto, non serve un "0"
// esplicito per un messaggio non voluto). Stessa convenzione di nome
// comando gia' confermata funzionante per 1005/1033/1074/1084/1094/1124
// in questo file, estesa qui a tutti gli altri numeri seguendo lo stesso
// schema standard RTCM3 - non verificata riga per riga sul manuale comandi
// Unicore UM98x per ogni singolo numero, ma coerente con quanto gia' in uso.
static esp_err_t send_rtcm_if(uart_port_t uart_num, bool enabled, int rtcm_type)
{
    if (!enabled) {
        return ESP_OK;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "RTCM%d 1", rtcm_type);
    return send_cmd(uart_num, cmd);
}

esp_err_t gnss_unicore_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore come base RTK (auto-survey + RTCM3)");

    app_settings_t s = settings_get();

    // UNLOG azzera tutti i log/messaggi attivi: non serve disattivare
    // esplicitamente un messaggio non scelto, parte gia' da zero.
    esp_err_t err = send_cmd(uart_num, "UNLOG");
    esp_err_t e = send_cmd(uart_num, "MODE BASE TIME 60 2.5");
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1005_enable, 1005);
    if (e != ESP_OK) err = e;
    e = send_cmd(uart_num, "RTCM1033 1"); // descrittore antenna+ricevitore Unicore, sempre utile, non fa parte della selezione (diverso da 1007/1008)
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1230_enable, 1230);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1007_enable, 1007);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1008_enable, 1008);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1019_enable, 1019);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1020_enable, 1020);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1074_enable, 1074);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1077_enable, 1077);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1084_enable, 1084);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1087_enable, 1087);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1094_enable, 1094);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1097_enable, 1097);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1124_enable, 1124);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1127_enable, 1127);
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
