#include "ntrip_caster_server.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

static const char *TAG = "ntrip_caster";

#define MAX_CLIENTS 4
#define CLIENT_SEND_TIMEOUT_MS 500
#define CLIENT_HANDSHAKE_TIMEOUT_S 5

static int s_client_socks[MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mutex;
static StreamBufferHandle_t s_feed_stream;

static void close_client_locked(int idx)
{
    if (s_client_socks[idx] >= 0) {
        close(s_client_socks[idx]);
        s_client_socks[idx] = -1;
    }
}

static void broadcast_task(void *arg)
{
    uint8_t buf[512];
    while (1) {
        size_t n = xStreamBufferReceive(s_feed_stream, buf, sizeof(buf), pdMS_TO_TICKS(1000));
        if (n == 0) {
            continue;
        }

        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_client_socks[i] < 0) {
                continue;
            }
            // Il socket ha un timeout di invio (impostato all'accettazione,
            // vedi sotto): un rover lento/bloccato ritarda al massimo di
            // CLIENT_SEND_TIMEOUT_MS questo giro, non blocca gli altri
            // client ne' il chiamante di ntrip_caster_server_feed()
            // (che scrive solo sullo stream buffer, mai direttamente qui).
            int sent = send(s_client_socks[i], buf, n, 0);
            if (sent < 0) {
                ESP_LOGW(TAG, "Rover %d: invio fallito (errno %d), disconnesso", i, errno);
                close_client_locked(i);
            }
        }
        xSemaphoreGive(s_clients_mutex);
    }
}

// Handshake NTRIP lato server: legge la richiesta HTTP-like del rover
// ("GET /<mountpoint> HTTP/1.1", con eventuale header Authorization),
// verifica mountpoint e credenziali, risponde "ICY 200 OK" (stesso
// formato di risposta gia' accettato dal client rover di questo stesso
// firmware, vedi ntrip_rover_client.c) se tutto combacia.
static bool handle_handshake(int sock, const app_settings_t *s)
{
    char req[512] = {0};
    int total = 0;
    while (total < (int) sizeof(req) - 1) {
        int r = recv(sock, req + total, sizeof(req) - 1 - total, 0);
        if (r <= 0) {
            return false;
        }
        total += r;
        req[total] = '\0';
        if (strstr(req, "\r\n\r\n")) {
            break;
        }
    }

    char method[8] = {0};
    char path[64] = {0};
    if (sscanf(req, "%7s %63s", method, path) != 2 || strcmp(method, "GET") != 0) {
        return false;
    }
    const char *mountpoint = (path[0] == '/') ? path + 1 : path;
    if (strcmp(mountpoint, s->ntrip_caster_server_mountpoint) != 0) {
        ESP_LOGW(TAG, "Rover ha chiesto un mountpoint sconosciuto: %s", mountpoint);
        return false;
    }

    if (s->ntrip_caster_server_username[0] || s->ntrip_caster_server_password[0]) {
        char credentials[128];
        int cred_len = snprintf(credentials, sizeof(credentials), "%s:%s",
                                 s->ntrip_caster_server_username, s->ntrip_caster_server_password);
        unsigned char b64[192];
        size_t b64_len = 0;
        mbedtls_base64_encode(b64, sizeof(b64) - 1, &b64_len, (const unsigned char *) credentials, (size_t) cred_len);
        b64[b64_len] = '\0';
        char expected[224];
        snprintf(expected, sizeof(expected), "Authorization: Basic %s", (char *) b64);
        if (!strstr(req, expected)) {
            ESP_LOGW(TAG, "Rover: credenziali mancanti o errate");
            return false;
        }
    }

    const char *resp = "ICY 200 OK\r\n\r\n";
    send(sock, resp, strlen(resp), 0);
    return true;
}

static void listen_task(void *arg)
{
    app_settings_t s = settings_get();

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Creazione socket di ascolto fallita: errno %d", errno);
        vTaskDelete(NULL);
        return;
    }
    int opt = 1;
    setsockopt(listen_sock, SOL_SOCKET, SO_REUSEADDR, &opt, sizeof(opt));

    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_addr.s_addr = htonl(INADDR_ANY),
        .sin_port = htons(s.ntrip_caster_server_port),
    };
    if (bind(listen_sock, (struct sockaddr *) &addr, sizeof(addr)) != 0) {
        ESP_LOGE(TAG, "Bind sulla porta %d fallito: errno %d", s.ntrip_caster_server_port, errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }
    if (listen(listen_sock, MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "Listen fallito: errno %d", errno);
        close(listen_sock);
        vTaskDelete(NULL);
        return;
    }

    ESP_LOGI(TAG, "Server caster NTRIP locale attivo sulla porta %d, mountpoint /%s",
             s.ntrip_caster_server_port, s.ntrip_caster_server_mountpoint);

    while (1) {
        struct sockaddr_in client_addr;
        socklen_t client_len = sizeof(client_addr);
        int client_sock = accept(listen_sock, (struct sockaddr *) &client_addr, &client_len);
        if (client_sock < 0) {
            continue;
        }

        // Timeout di ricezione durante l'handshake: un client connesso ma
        // che non manda nulla (o troppo poco) non deve poter bloccare
        // questo task, impedendo ad altri rover di collegarsi.
        struct timeval rcv_tv = { .tv_sec = CLIENT_HANDSHAKE_TIMEOUT_S, .tv_usec = 0 };
        setsockopt(client_sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));

        app_settings_t cur = settings_get(); // rilegge: la configurazione puo' essere cambiata da avvio
        if (!handle_handshake(client_sock, &cur)) {
            close(client_sock);
            continue;
        }

        struct timeval snd_tv = {
            .tv_sec = CLIENT_SEND_TIMEOUT_MS / 1000,
            .tv_usec = (CLIENT_SEND_TIMEOUT_MS % 1000) * 1000,
        };
        setsockopt(client_sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));

        xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
        bool added = false;
        for (int i = 0; i < MAX_CLIENTS; i++) {
            if (s_client_socks[i] < 0) {
                s_client_socks[i] = client_sock;
                added = true;
                break;
            }
        }
        xSemaphoreGive(s_clients_mutex);

        if (!added) {
            ESP_LOGW(TAG, "Troppi rover gia' collegati (max %d), nuova connessione rifiutata", MAX_CLIENTS);
            close(client_sock);
        } else {
            ESP_LOGI(TAG, "Rover collegato al caster locale");
        }
    }
}

void ntrip_caster_server_start(void)
{
    app_settings_t s = settings_get();
    if (!s.ntrip_caster_server_enable) {
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        s_client_socks[i] = -1;
    }
    s_clients_mutex = xSemaphoreCreateMutex();
    s_feed_stream = xStreamBufferCreate(4096, 1);

    xTaskCreate(broadcast_task, "ntrip_cst_bc", 4096, NULL, 5, NULL);
    xTaskCreate(listen_task, "ntrip_cst_listen", 4096, NULL, 5, NULL);
}

void ntrip_caster_server_feed(const uint8_t *data, size_t len)
{
    if (!s_feed_stream) {
        return;
    }
    // Non bloccante: se il buffer e' pieno (nessun consumatore, o
    // broadcast_task in ritardo) i byte in eccesso vengono scartati
    // invece di rallentare il chiamante (gnss_uart_task, sul percorso
    // critico verso il caster esterno).
    xStreamBufferSend(s_feed_stream, data, len, 0);
}

size_t ntrip_caster_server_get_client_count(void)
{
    if (!s_clients_mutex) {
        return 0;
    }
    size_t n = 0;
    xSemaphoreTake(s_clients_mutex, portMAX_DELAY);
    for (int i = 0; i < MAX_CLIENTS; i++) {
        if (s_client_socks[i] >= 0) {
            n++;
        }
    }
    xSemaphoreGive(s_clients_mutex);
    return n;
}
