#include "ntrip_rover_client.h"
#include "settings.h"
#include "status.h"

#include <string.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "driver/uart.h"
#include "mbedtls/base64.h"

static const char *TAG = "ntrip_rover";

// Socket attivo (-1 se non connesso), condiviso tra ntrip_rover_client_task
// e ntrip_rover_client_forward_gga() (chiamata dal task gnss_nmea_reader).
static volatile int s_sock = -1;
static SemaphoreHandle_t s_sock_mutex;

// Handshake NTRIP client "GET": richiede la mountpoint con autenticazione
// Basic. Accetta sia la risposta NTRIP 1.0 ("ICY 200 OK") sia quella
// NTRIP 2.0/HTTP ("HTTP/1.1 200 OK") - va verificato quale usa realmente
// il caster EVONETRTK.
static int ntrip_rover_connect(const app_settings_t *settings)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", settings->ntrip_port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(settings->ntrip_host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup fallita per %s", settings->ntrip_host);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Creazione socket fallita: errno %d", errno);
        freeaddrinfo(res);
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Connessione a %s:%d fallita: errno %d", settings->ntrip_host, settings->ntrip_port, errno);
        close(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    char credentials[112];
    int cred_len = snprintf(credentials, sizeof(credentials), "%s:%s",
                             settings->ntrip_username, settings->ntrip_password);

    unsigned char b64[160];
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64) - 1, &b64_len, (const unsigned char *) credentials, cred_len);
    b64[b64_len] = '\0';

    char req[320];
    int req_len = snprintf(req, sizeof(req),
        "GET /%s HTTP/1.1\r\n"
        "User-Agent: NTRIP baseesp32/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        settings->ntrip_mountpoint, (const char *) b64);

    if (send(sock, req, req_len, 0) != req_len) {
        ESP_LOGE(TAG, "Invio richiesta NTRIP GET fallito: errno %d", errno);
        close(sock);
        return -1;
    }

    char resp[128] = {0};
    int r = recv(sock, resp, sizeof(resp) - 1, 0);
    if (r <= 0) {
        ESP_LOGE(TAG, "Nessuna risposta dal caster");
        close(sock);
        return -1;
    }
    resp[r] = '\0';
    if (strncmp(resp, "ICY 200", 7) != 0 && strncmp(resp, "HTTP/1.1 200", 12) != 0) {
        ESP_LOGE(TAG, "Caster ha rifiutato la richiesta rover: %s", resp);
        close(sock);
        return -1;
    }

    ESP_LOGI(TAG, "Rover connesso al caster %s:%d mountpoint /%s",
             settings->ntrip_host, settings->ntrip_port, settings->ntrip_mountpoint);
    return sock;
}

static void set_active_sock(int sock)
{
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    s_sock = sock;
    xSemaphoreGive(s_sock_mutex);
}

void ntrip_rover_client_task(void *arg)
{
    uart_port_t uart_num = (uart_port_t)(intptr_t) arg;
    uint8_t net_buf[512];

    if (!s_sock_mutex) {
        s_sock_mutex = xSemaphoreCreateMutex();
    }

    while (1) {
        app_settings_t settings = settings_get();
        int sock = ntrip_rover_connect(&settings);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        set_active_sock(sock);

        while (1) {
            int n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n > 0) {
                uart_write_bytes(uart_num, (const char *) net_buf, n);
                status_note_rtcm_bytes((uint32_t) n);
            } else if (n == 0) {
                ESP_LOGW(TAG, "Caster ha chiuso la connessione");
                break;
            } else {
                ESP_LOGW(TAG, "recv fallita, riconnessione: errno %d", errno);
                break;
            }
        }

        set_active_sock(-1);
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}

void ntrip_rover_client_forward_gga(const char *line, size_t len)
{
    if (!s_sock_mutex) {
        return;
    }

    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    int sock = s_sock;
    xSemaphoreGive(s_sock_mutex);

    if (sock < 0 || len > 100) {
        return;
    }

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%.*s\r\n", (int) len, line);
    send(sock, buf, n, 0);
}
