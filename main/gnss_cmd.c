#include "gnss_cmd.h"
#include "gnss_io.h"
#include "gnss_fix.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

esp_err_t gnss_cmd_send_line(const char *tag, const char *cmd)
{
    ESP_LOGI(tag, "-> %s", cmd);
    int len = (int) strlen(cmd);
    int written = gnss_io_write(cmd, len);
    gnss_io_write("\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}

esp_err_t gnss_cmd_send_nmea(const char *tag, const char *body)
{
    char line[128];
    int len = snprintf(line, sizeof(line), "$%s*%02X\r\n", body, nmea_checksum(body));
    // Riga troncata: snprintf ritorna la lunghezza che avrebbe voluto
    // scrivere, mandarne len byte leggerebbe oltre la fine di line.
    if (len <= 0 || len >= (int) sizeof(line)) {
        return ESP_ERR_INVALID_SIZE;
    }
    ESP_LOGI(tag, "-> %.*s", len - 2, line);
    int written = gnss_io_write(line, len);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}
