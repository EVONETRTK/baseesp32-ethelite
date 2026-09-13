#include <stdint.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

#include "esp_log.h"
#include "nvs_flash.h"
#include "driver/uart.h"

#include "sdkconfig.h"
#include "settings.h"
#include "status.h"
#include "status_led.h"
#include "oled_display.h"
#include "reset_button.h"
#include "net_manager.h"
#include "web_ui.h"
#include "gnss_driver.h"
#include "gnss_signal.h"
#include "nmea_udp_broadcast.h"
#include "gnss_nmea_reader.h"
#include "ntrip_client.h"
#include "ntrip_rover_client.h"

#define UART_RX_BUF_SIZE 1024

static StreamBufferHandle_t rtcm_stream;
static uart_port_t s_gnss_uart_num;

// A differenza dei pin del modem/Ethernet (fissati dallo shield, restano
// solo in Kconfig), i pin verso il GNSS esterno variano per installazione
// e sono configurabili a runtime dalla UI web (scheda Hardware) - vedi
// settings.h. Qui si leggono i valori effettivi, non le macro Kconfig
// (che restano solo come default iniziale in settings.c).
static void gnss_uart_init(const app_settings_t *settings)
{
    s_gnss_uart_num = (uart_port_t) settings->gnss_uart_num;

    uart_config_t uart_config = {
        .baud_rate = settings->gnss_uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(s_gnss_uart_num, UART_RX_BUF_SIZE * 2, UART_RX_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(s_gnss_uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(s_gnss_uart_num, settings->gnss_uart_tx_pin, settings->gnss_uart_rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

// Modalita' BASE: legge i byte RTCM3 grezzi emessi dal GNSS e li inoltra
// al task NTRIP tramite stream buffer. Nessun parsing dei frame: il GNSS
// deve gia' emettere RTCM3 valido (modalita' base fissata sul ricevitore).
static void gnss_uart_task(void *arg)
{
    uint8_t buf[UART_RX_BUF_SIZE];
    while (1) {
        int len = uart_read_bytes(s_gnss_uart_num, buf, sizeof(buf), pdMS_TO_TICKS(100));
        if (len > 0) {
            status_note_rtcm_bytes((uint32_t) len);
            xStreamBufferSend(rtcm_stream, buf, len, pdMS_TO_TICKS(1000));
        }
    }
}

void app_main(void)
{
    esp_err_t nvs_err = nvs_flash_init();
    if (nvs_err == ESP_ERR_NVS_NO_FREE_PAGES || nvs_err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        nvs_err = nvs_flash_init();
    }
    ESP_ERROR_CHECK(nvs_err);

    settings_init();
    gnss_signal_init();

    status_led_start();
    oled_display_start();
    reset_button_start();

    // Porta su l'AP di setup + tenta WiFi/cellulare/Ethernet in background
    // (non blocca): la UI web deve restare raggiungibile anche senza rete
    // configurata.
    net_manager_start();
    nmea_udp_broadcast_init();
    web_ui_start();

    app_settings_t settings = settings_get();
    gnss_uart_init(&settings);
    gnss_driver_configure(s_gnss_uart_num, settings.gnss_chip, settings.device_mode);

    if (settings.device_mode == DEVICE_MODE_ROVER) {
        // Rover: un solo task legge la UART e smista lo stream NMEA a
        // broadcast UDP (AgOpenGPS/AgIO), inoltro GGA al caster, e stato
        // satelliti per la UI (vedi gnss_nmea_reader.c). Il client NTRIP
        // scrive solo l'RTCM3 ricevuto verso il GNSS, non tocca piu' la
        // UART in lettura. Niente Bluetooth Classic su questa scheda
        // (ESP32-S3 non lo supporta, solo BLE non implementata qui): su
        // iOS/qualunque piattaforma resta il broadcast UDP via WiFi/Ethernet.
        xTaskCreate(gnss_nmea_reader_task, "nmea_reader", 4096,
                    (void *)(intptr_t) s_gnss_uart_num, 6, NULL);
        xTaskCreate(ntrip_rover_client_task, "ntrip_rover", 4096,
                    (void *)(intptr_t) s_gnss_uart_num, 5, NULL);
    } else {
        rtcm_stream = xStreamBufferCreate(4096, 1);
        xTaskCreate(gnss_uart_task, "gnss_uart", 4096, NULL, 10, NULL);
        xTaskCreate(ntrip_client_task, "ntrip_client", 4096, rtcm_stream, 5, NULL);
    }
}
