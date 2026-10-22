#include "gnss_l76k.h"
#include "gnss_io.h"
#include "gnss_cmd.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gnss_l76k";

// Manda "$<body>*<checksum>\r\n": checksum calcolato da gnss_cmd_send_nmea
// invece che scritto a mano accanto a ogni comando.
static esp_err_t send_pcas(uart_port_t uart_num, const char *body)
{
    (void) uart_num;
    return gnss_cmd_send_nmea(TAG, body);
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
