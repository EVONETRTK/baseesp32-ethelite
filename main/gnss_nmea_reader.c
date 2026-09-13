#include "gnss_nmea_reader.h"
#include "nmea_udp_broadcast.h"
#include "gnss_signal.h"
#include "gnss_fix.h"
#include "ntrip_rover_client.h"

#include <string.h>
#include <stdint.h>

#include "driver/uart.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define READ_BUF_SIZE 256
#define LINE_BUF_SIZE 128

void gnss_nmea_reader_task(void *arg)
{
    uart_port_t uart_num = (uart_port_t)(intptr_t) arg;
    uint8_t read_buf[READ_BUF_SIZE];
    char line[LINE_BUF_SIZE];
    size_t line_len = 0;

    while (1) {
        int n = uart_read_bytes(uart_num, read_buf, sizeof(read_buf), pdMS_TO_TICKS(100));
        for (int i = 0; i < n; i++) {
            char c = (char) read_buf[i];

            if (c == '\n') {
                if (line_len > 6 && line[0] == '$') {
                    line[line_len] = '\0';

                    nmea_udp_broadcast_send(line, line_len);

                    // Formato NMEA: '$' + talker (2 char) + tipo sentenza
                    // (3 char), es. "$GPGGA"/"$GNGGA"/"$GPGSV"...
                    if (memcmp(&line[3], "GGA", 3) == 0) {
                        ntrip_rover_client_forward_gga(line, line_len);
                        gnss_fix_parse_gga(line);
                    } else if (memcmp(&line[3], "GSV", 3) == 0) {
                        gnss_signal_parse_gsv(line);
                    }
                }
                line_len = 0;
            } else if (c != '\r') {
                if (line_len < LINE_BUF_SIZE - 1) {
                    line[line_len++] = (char) c;
                } else {
                    // Riga troppo lunga (improbabile per NMEA standard):
                    // scarta e riparte dalla prossima, per non restare
                    // bloccati a concatenare spazzatura all'infinito.
                    line_len = 0;
                }
            }
        }
    }
}
