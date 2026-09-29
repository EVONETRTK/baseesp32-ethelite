#include <string.h>
#include <errno.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/task.h"
#include "esp_log.h"

#include "ntrip_client.h"
#include "settings.h"
#include "status.h"
#include "net_util.h"
#include "ntrip_reply.h"

static const char *TAG = "ntrip_client";

// Handshake NTRIP 1.0 "source" (SOURCE <password> /<mountpoint>) verso il
// caster. Usato sia dal task della base sia dal pulsante "Prova connessione"
// del pannello (ntrip_client_test_source), cosi' la prova fa esattamente la
// stessa cosa della base vera. Ritorna il socket collegato, oppure -1 con il
// motivo in err (frase leggibile, vedi ntrip_reply.h).
static int source_handshake(const char *host, uint16_t port, const char *mountpoint,
                            const char *password, int timeout_s, char *err, size_t err_size)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    int rc = getaddrinfo(host, port_str, &hints, &res);
    if (rc != 0 || res == NULL) {
        snprintf(err, err_size, "Indirizzo del caster non trovato (DNS): %s", host);
        return -1;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        snprintf(err, err_size, "Creazione socket fallita (errno %d)", errno);
        return -1;
    }
    if (timeout_s > 0) {
        struct timeval tv = { .tv_sec = timeout_s, .tv_usec = 0 };
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    }

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        snprintf(err, err_size, "Connessione a %s:%u fallita (errno %d)", host, port, errno);
        net_close_now(sock);
        freeaddrinfo(res);
        return -1;
    }
    freeaddrinfo(res);

    char req[256];
    int req_len = snprintf(req, sizeof(req),
        "SOURCE %s /%s\r\n"
        "Source-Agent: NTRIP baseesp32/1.0\r\n"
        "\r\n",
        password, mountpoint);
    if (send(sock, req, req_len, 0) != req_len) {
        snprintf(err, err_size, "Invio richiesta al caster fallito (errno %d)", errno);
        net_close_now(sock);
        return -1;
    }

    char resp[128] = {0};
    int r = recv(sock, resp, sizeof(resp) - 1, 0);
    if (r <= 0) {
        snprintf(err, err_size, "Nessuna risposta dal caster");
        net_close_now(sock);
        return -1;
    }
    resp[r] = '\0';
    if (strncmp(resp, "ICY 200", 7) != 0 && strncmp(resp, "OK", 2) != 0) {
        char *eol = strpbrk(resp, "\r\n");
        if (eol) {
            *eol = '\0'; // solo la prima riga della risposta
        }
        const char *why = ntrip_explain_reply(resp, true);
        if (why) {
            snprintf(err, err_size, "Caster ha rifiutato: %s", why);
        } else {
            snprintf(err, err_size, "Caster ha rifiutato: %.60s", resp);
        }
        net_close_now(sock);
        return -1;
    }
    return sock;
}

static int ntrip_connect_and_handshake(const app_settings_t *settings)
{
    char err[200];
    int sock = source_handshake(settings->ntrip_host, settings->ntrip_port, settings->ntrip_mountpoint,
                                settings->ntrip_password, 10, err, sizeof(err));
    if (sock < 0) {
        ESP_LOGE(TAG, "%s", err);
        status_ntrip_note_disconnected(err);
        return -1;
    }
    // I timeout servivano solo all'handshake: per l'invio dei dati resta il
    // comportamento di prima (bloccante, errori gestiti dal task).
    struct timeval none = { 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &none, sizeof(none));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &none, sizeof(none));

    ESP_LOGI(TAG, "Connesso al caster %s:%d mountpoint /%s",
             settings->ntrip_host, settings->ntrip_port, settings->ntrip_mountpoint);
    status_ntrip_note_connected();
    return sock;
}

bool ntrip_client_test_source(const char *host, uint16_t port, const char *mountpoint,
                              const char *password, char *out_msg, size_t out_msg_size)
{
    int sock = source_handshake(host, port, mountpoint, password, 6, out_msg, out_msg_size);
    if (sock < 0) {
        return false;
    }
    net_close_now(sock);
    snprintf(out_msg, out_msg_size, "Caster ha accettato la base sulla mountpoint /%s", mountpoint);
    return true;
}

void ntrip_client_task(void *arg)
{
    StreamBufferHandle_t rtcm_stream = (StreamBufferHandle_t) arg;
    uint8_t buf[512];
    // Attesa tra un tentativo fallito e l'altro: 5 s, poi raddoppia fino a
    // 60 s finche' il caster continua a rifiutare (mountpoint inesistente,
    // password sbagliata...), per non martellarlo ne' consumare risorse di
    // rete ogni 5 s all'infinito. Torna a 5 s appena una connessione riesce.
    uint32_t retry_ms = 5000;

    while (1) {
        app_settings_t settings = settings_get();
        int sock = ntrip_connect_and_handshake(&settings);
        if (sock < 0) {
            vTaskDelay(pdMS_TO_TICKS(retry_ms));
            retry_ms = (retry_ms * 2 > 60000) ? 60000 : retry_ms * 2;
            continue;
        }
        retry_ms = 5000;

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
        net_close_now(sock);
        vTaskDelay(pdMS_TO_TICKS(2000));
    }
}
