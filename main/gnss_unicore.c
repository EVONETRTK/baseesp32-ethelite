#include "gnss_unicore.h"

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

esp_err_t gnss_unicore_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore come base RTK (auto-survey + RTCM3)");

    // Sequenza comandi ASCII stile Unicore UM98x: azzera i log attivi,
    // imposta posizione base con auto-survey (durata 60s, precisione
    // richiesta 2.5m), poi abilita i messaggi RTCM3 principali a 1Hz
    // sulla porta corrente.
    static const char *const cmds[] = {
        "UNLOG",
        "MODE BASE TIME 60 2.5",
        "RTCM1005 1",
        "RTCM1033 1",
        "RTCM1074 1",
        "RTCM1084 1",
        "RTCM1094 1",
        "RTCM1124 1",
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
