#include "gnss_l76k.h"
#include "gnss_io.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gnss_l76k";

// Manda "$<body>*<checksum>\r\n": checksum NMEA standard (XOR di tutti i
// caratteri tra '$' e '*'), calcolato qui invece che scritto a mano accanto
// a ogni comando.
static esp_err_t send_pcas(uart_port_t uart_num, const char *body)
{
    uint8_t ck = 0;
    for (const char *p = body; *p; p++) {
        ck ^= (uint8_t) *p;
    }
    char cmd[64];
    int len = snprintf(cmd, sizeof(cmd), "$%s*%02X\r\n", body, ck);
    if (len <= 0 || len >= (int) sizeof(cmd)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(TAG, "-> %.*s", len - 2, cmd);
    int written = gnss_io_write(cmd, len);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t gnss_l76k_configure_base(uart_port_t uart_num)
{
    (void) uart_num;
    ESP_LOGE(TAG, "Quectel L76K non puo' fare da base: non e' un ricevitore RTK e non produce RTCM. "
                  "Usarlo solo in modalita' rover per le prove, oppure collegare il ricevitore RTK.");
    return ESP_ERR_NOT_SUPPORTED;
}

esp_err_t gnss_l76k_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro Quectel L76K per le prove rover (GPS + BeiDou + GLONASS, niente RTK)");

    // PCAS04 = costellazioni: 1 GPS, 2 BeiDou, 4 GLONASS, sommabili (7 = tutte e tre).
    return send_pcas(uart_num, "PCAS04,7");
}
