#include "ntrip_caster_server.h"
#include "license.h"
#include "settings.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <sys/socket.h>
#include <netinet/in.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "esp_heap_caps.h"
#include "freertos/stream_buffer.h"
#include "freertos/semphr.h"
#include "mbedtls/base64.h"

static const char *TAG = "ntrip_caster";

#define MAX_CLIENTS 4
#define CLIENT_SEND_TIMEOUT_MS 500
#define CLIENT_HANDSHAKE_TIMEOUT_S 5

static int s_client_socks[MAX_CLIENTS];
// Client NTRIP 2.0: i dati vanno in "chunked transfer encoding" (HTTP/1.1).
static bool s_client_v2[MAX_CLIENTS];
static SemaphoreHandle_t s_clients_mutex;
static StreamBufferHandle_t s_feed_stream;

static void close_client_locked(int idx)
{
    if (s_client_socks[idx] >= 0) {
        close(s_client_socks[idx]);
        s_client_socks[idx] = -1;
    }
}

// Invia tutto il blocco entro deadline_us (esp_timer). Con il timeout di
// invio del socket, send() puo' scrivere solo una parte allo scadere del
// tempo: prima il resto veniva perso, e per i client NTRIP 2.0 (dati "a
// blocchi") il formato si rompeva per sempre (dimensione del blocco
// sbagliata, client che riceve solo dati illeggibili). false = client da
// chiudere (il flusso verso di lui non e' piu' integro).
static bool client_send_all(int sock, const void *data, size_t len, int64_t deadline_us)
{
    const uint8_t *p = (const uint8_t *) data;
    size_t off = 0;
    while (off < len) {
        int sent = send(sock, p + off, len - off, 0);
        if (sent <= 0) {
            return false;
        }
        off += (size_t) sent;
        if (off < len && esp_timer_get_time() > deadline_us) {
            return false;
        }
    }
    return true;
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
            // ~3 x CLIENT_SEND_TIMEOUT_MS questo giro, poi viene chiuso; non
            // blocca il chiamante di ntrip_caster_server_feed() (che scrive
            // solo sullo stream buffer, mai direttamente qui).
            int64_t deadline = esp_timer_get_time() + 3LL * CLIENT_SEND_TIMEOUT_MS * 1000;
            bool ok;
            if (s_client_v2[i]) {
                char hdr[12];
                int hl = snprintf(hdr, sizeof(hdr), "%X\r\n", (unsigned) n);
                ok = client_send_all(s_client_socks[i], hdr, (size_t) hl, deadline) &&
                     client_send_all(s_client_socks[i], buf, n, deadline) &&
                     client_send_all(s_client_socks[i], "\r\n", 2, deadline);
            } else {
                ok = client_send_all(s_client_socks[i], buf, n, deadline);
            }
            if (!ok) {
                ESP_LOGW(TAG, "Rover %d: invio fallito o incompleto (errno %d), disconnesso", i, errno);
                close_client_locked(i);
            }
        }
        xSemaphoreGive(s_clients_mutex);
    }
}

// Tabella delle sorgenti (una riga STR per il mountpoint di questa base):
// le app dei rover la chiedono con "GET /" per mostrare l'elenco dei
// mountpoint. Prima il caster chiudeva la connessione senza risposta e il
// mountpoint andava scritto a mano (collaudo del 03/10/2026).
static void send_sourcetable(int sock, const app_settings_t *s, bool v2)
{
    char body[320];
    bool auth = s->ntrip_caster_server_username[0] || s->ntrip_caster_server_password[0];
    double lat = s->base_position_mode == BASE_POSITION_MANUAL ? s->base_fixed_lat_deg : 0.0;
    double lon = s->base_position_mode == BASE_POSITION_MANUAL ? s->base_fixed_lon_deg : 0.0;
    int bl = snprintf(body, sizeof(body),
                      "STR;%s;%s;RTCM 3.3;1005(10),1077(1),1087(1),1097(1),1127(1),1230(1);2;GPS+GLO+GAL+BDS;"
                      "EVONETRTK;ITA;%.4f;%.4f;0;0;EVONETRTK ESP32;none;%s;N;0;\r\nENDSOURCETABLE\r\n",
                      s->ntrip_caster_server_mountpoint, s->ntrip_caster_server_mountpoint, lat, lon, auth ? "B" : "N");
    if (bl < 0 || bl >= (int) sizeof(body)) {
        bl = (int) strlen(body);
    }
    char head[192];
    int hl = v2 ? snprintf(head, sizeof(head),
                           "HTTP/1.1 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nServer: EVONETRTK\r\n"
                           "Content-Type: gnss/sourcetable\r\nContent-Length: %d\r\nConnection: close\r\n\r\n", bl)
                : snprintf(head, sizeof(head),
                           "SOURCETABLE 200 OK\r\nServer: EVONETRTK\r\nContent-Type: text/plain\r\n"
                           "Content-Length: %d\r\n\r\n", bl);
    send(sock, head, hl, 0);
    send(sock, body, bl, 0);
}

static void send_simple(int sock, bool v2, const char *status, const char *extra)
{
    char head[224];
    int hl = snprintf(head, sizeof(head), "%s %s\r\n%sServer: EVONETRTK\r\n%sConnection: close\r\n\r\n",
                      v2 ? "HTTP/1.1" : "HTTP/1.0", status, v2 ? "Ntrip-Version: Ntrip/2.0\r\n" : "", extra ? extra : "");
    send(sock, head, hl, 0);
}

// Handshake NTRIP lato server: legge la richiesta del rover ("GET
// /<mountpoint>", con eventuale Authorization e "Ntrip-Version: Ntrip/2.0"),
// risponde secondo la versione chiesta. *out_v2 dice se i dati vanno
// inviati a blocchi (NTRIP 2.0). Rispondono sempre qualcosa: tabella delle
// sorgenti, 401 o 404, invece di chiudere in silenzio.
static bool handle_handshake(int sock, const app_settings_t *s, bool *out_v2)
{
    char req[512] = {0};
    int total = 0;
    // Limite di tempo per TUTTA la richiesta, non per ogni singola recv():
    // prima un client che mandava un byte ogni 4 s teneva occupato questo
    // task (e quindi impediva agli altri rover di collegarsi) per decine di
    // minuti.
    int64_t deadline = esp_timer_get_time() + (int64_t) CLIENT_HANDSHAKE_TIMEOUT_S * 1000000;
    while (total < (int) sizeof(req) - 1) {
        int64_t left_us = deadline - esp_timer_get_time();
        if (left_us <= 0) {
            return false;
        }
        struct timeval tv;
        tv.tv_sec = left_us / 1000000;
        tv.tv_usec = left_us % 1000000;
        setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
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
    bool v2 = strcasestr(req, "Ntrip-Version: Ntrip/2.0") != NULL;
    *out_v2 = v2;
    const char *mountpoint = (path[0] == '/') ? path + 1 : path;
    if (mountpoint[0] == '\0') {
        send_sourcetable(sock, s, v2);
        return false;
    }
    if (strcmp(mountpoint, s->ntrip_caster_server_mountpoint) != 0) {
        ESP_LOGW(TAG, "Rover ha chiesto un mountpoint sconosciuto: %s", mountpoint);
        // NTRIP 1: a un mountpoint sconosciuto si risponde con la tabella.
        if (v2) {
            send_simple(sock, true, "404 Not Found", NULL);
        } else {
            send_sourcetable(sock, s, false);
        }
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
        // Nome dell'intestazione senza badare alle maiuscole, credenziali
        // (base64) confrontate in modo esatto: con strcasestr su tutta la
        // riga una password diversa solo per maiuscole passava.
        const char *h = strcasestr(req, "\nAuthorization:");
        bool ok = false;
        if (h) {
            h += strlen("\nAuthorization:");
            while (*h == ' ') {
                h++;
            }
            if (strncasecmp(h, "Basic ", 6) == 0) {
                h += 6;
                while (*h == ' ') {
                    h++;
                }
                ok = strncmp(h, (const char *) b64, b64_len) == 0 &&
                     (h[b64_len] == '\r' || h[b64_len] == '\n' || h[b64_len] == ' ' || h[b64_len] == '\0');
            }
        }
        if (!ok) {
            ESP_LOGW(TAG, "Rover: credenziali mancanti o errate");
            char extra[96];
            snprintf(extra, sizeof(extra), "WWW-Authenticate: Basic realm=\"/%s\"\r\n", s->ntrip_caster_server_mountpoint);
            send_simple(sock, v2, "401 Unauthorized", extra);
            return false;
        }
    }

    const char *resp = v2 ? "HTTP/1.1 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nServer: EVONETRTK\r\n"
                            "Content-Type: gnss/data\r\nCache-Control: no-store\r\nTransfer-Encoding: chunked\r\n"
                            "Connection: close\r\n\r\n"
                          : "ICY 200 OK\r\n\r\n";
    send(sock, resp, strlen(resp), 0);
    return true;
}

// Il task ha lo stack in PSRAM (xTaskCreateWithCaps): vTaskDelete(NULL) non
// e' sicuro. Se il caster locale non puo' partire il task resta fermo.
static void listen_fail_park(void)
{
    for (;;) {
        vTaskDelay(portMAX_DELAY);
    }
}

static void listen_task(void *arg)
{
    // app_settings_t pesa ~1.7 KB. Con due copie intere sullo stack (questa
    // e quella per ogni rover sotto) piu' i buffer dell'handshake il task
    // andava in stack overflow appena attivato il caster locale: riavvii a
    // ripetizione (visto sul dispositivo). Statiche: un solo task le usa.
    static app_settings_t s;
    settings_get_into(&s); // niente copia temporanea di settings_get() sullo stack

    int listen_sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (listen_sock < 0) {
        ESP_LOGE(TAG, "Creazione socket di ascolto fallita: errno %d", errno);
        listen_fail_park();
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
        listen_fail_park();
        return;
    }
    if (listen(listen_sock, MAX_CLIENTS) != 0) {
        ESP_LOGE(TAG, "Listen fallito: errno %d", errno);
        close(listen_sock);
        listen_fail_park();
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

        // Rilegge: la configurazione puo' essere cambiata dall'avvio. Riusa
        // la copia statica di sopra (porta e mountpoint servono solo
        // all'apertura del socket): una copia in meno, 2,1 KB di RAM.
        settings_get_into(&s);
        bool v2 = false;
        if (!handle_handshake(client_sock, &s, &v2)) {
            // Lascia partire la risposta (tabella, 401, 404) prima di chiudere.
            shutdown(client_sock, SHUT_WR);
            vTaskDelay(pdMS_TO_TICKS(100));
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
                s_client_v2[i] = v2;
                added = true;
                break;
            }
        }
        xSemaphoreGive(s_clients_mutex);

        if (!added) {
            ESP_LOGW(TAG, "Troppi rover gia' collegati (max %d), nuova connessione rifiutata", MAX_CLIENTS);
            close(client_sock);
        } else {
            ESP_LOGI(TAG, "Rover collegato al caster locale (NTRIP %s)", v2 ? "2.0" : "1.0");
        }
    }
}

static void peek_enable(const app_settings_t *s, void *ctx)
{
    *(bool *) ctx = s->ntrip_caster_server_enable && license_has(LIC_BASE);
}

void ntrip_caster_server_start(void)
{
    // Solo l'interruttore: chiamata dal task main all'avvio, niente copia
    // della configurazione (2,4 KB) sul suo stack (vedi settings_get_into).
    bool enable = false;
    settings_peek(peek_enable, &enable);
    if (!enable) {
        return;
    }

    for (int i = 0; i < MAX_CLIENTS; i++) {
        s_client_socks[i] = -1;
    }
    s_clients_mutex = xSemaphoreCreateMutex();
    s_feed_stream = xStreamBufferCreate(4096, 1);

    // Stack in PSRAM (RAM interna scarsa): questi task non scrivono mai in flash/NVS.
    if (xTaskCreateWithCaps(broadcast_task, "ntrip_cst_bc", 4096, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        xTaskCreate(broadcast_task, "ntrip_cst_bc", 4096, NULL, 5, NULL);
    }
    // 8192 (era 6144): con tabella delle sorgenti e risposte NTRIP 2.0 restavano
    // 1220 byte liberi (05/10/2026).
    if (xTaskCreateWithCaps(listen_task, "ntrip_cst_listen", 8192, NULL, 5, NULL, MALLOC_CAP_SPIRAM) != pdPASS) {
        xTaskCreate(listen_task, "ntrip_cst_listen", 8192, NULL, 5, NULL);
    }
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
