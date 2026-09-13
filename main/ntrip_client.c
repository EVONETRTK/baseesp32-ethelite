#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/task.h"
#include "esp_log.h"

#include "ntrip_client.h"
#include "settings.h"
#include "status.h"

static const char *TAG = "ntrip_client";

// Handshake NTRIP 1.0 "source": funziona con la maggior parte dei caster,
// ma va verificato contro l'implementazione reale del caster EVONETRTK
// (formato risposta attesa, eventuale differenza NTRIP 2.0/HTTP).
static int ntrip_connect_and_handshake(const app_settings_t *settings)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", settings->ntrip_port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int err = getaddrinfo(settings->ntrip_host, port_str, &hints, &res);
    if (err != 0 || res == NULL) {
        ESP_LOGE(TAG, "DNS lookup fallita per %s: %d", settings->ntrip_host, err);
        status_ntrip_note_disconnected("DNS lookup fallita");
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        ESP_LOGE(TAG, "Creazione socket fallita: errno %d", errno);
        freeaddrinfo(res);
        status_ntrip_note_disconnected("Creazione socket fallita");
        return -1;
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGE(TAG, "Connessione a %s:%d fallita: errno %d", settings->ntrip_host, settings->ntrip_port, errno);
        close(sock);
        freeaddrinfo(res);
        char msg[128];
        snprintf(msg, sizeof(msg), "Connessione al caster fallita (errno %d)", errno);
        status_ntrip_note_disconnected(msg);
        return -1;
    }
    freeaddrinfo(res);

    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "SOURCE %s /%s\r\n"
        "Source-Agent: NTRIP baseesp32/1.0\r\n"
        "\r\n",
        settings->ntrip_password, settings->ntrip_mountpoint);
    if (send(sock, req, req_len, 0) != req_len) {
        ESP_LOGE(TAG, "Invio handshake NTRIP fallito: errno %d", errno);
        close(sock);
        status_ntrip_note_disconnected("Invio handshake fallito");
        return -1;
    }

    char resp[128] = {0};
    int r = recv(sock, resp, sizeof(resp) - 1, 0);
    if (r <= 0) {
        ESP_LOGE(TAG, "Nessuna risposta dal caster");
        close(sock);
        status_ntrip_note_disconnected("Nessuna risposta dal caster");
        return -1;
    }
    resp[r] = '\0';
    if (strncmp(resp, "ICY 200", 7) != 0 && strncmp(resp, "OK", 2) != 0) {
        ESP_LOGE(TAG, "Caster ha rifiutato la connessione sorgente: %s", resp);
        close(sock);
        char msg[128];
        snprintf(msg, sizeof(msg), "Caster ha rifiutato la connessione: %.50s", resp);
        status_ntrip_note_disconnected(msg);
        return -1;
    }

    ESP_LOGI(TAG, "Connesso al caster %s:%d mountpoint /%s",
             settings->ntrip_host, settings->ntrip_port, settings->ntrip_mountpoint);
    status_ntrip_note_connected();
    return sock;
}

void ntrip_client_task(void *arg)
{
    StreamBufferHandle_t rtcm_stream = (StreamBufferHandle_t) arg;
    uint8_t buf[512];

    while (1) {
        app_settings_t settings = settings_get();
        int sock = ntrip_connect_and_handshake(&settings);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(5000));
            continue;
        }

        while (1) {
            size_t len = xStreamBufferReceive(rtcm_stream, buf, sizeof(buf), pdMS_TO_TICKS(1000));
            if (len == 0) {
                continue;
            }
            int sent = send(sock, buf, len, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "Invio fallito, riconnessione: errno %d", errno);
                char msg[128];
                snprintf(msg, sizeof(msg), "Invio dati fallito, riconnessione (errno %d)", errno);
                status_ntrip_note_disconnected(msg);
                break;
            }
        }
        close(sock);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
