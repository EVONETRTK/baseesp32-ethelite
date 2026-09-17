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
        status_ntrip_note_disconnected("Invio richiesta fallito");
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
    if (strncmp(resp, "ICY 200", 7) != 0 && strncmp(resp, "HTTP/1.1 200", 12) != 0) {
        ESP_LOGE(TAG, "Caster ha rifiutato la richiesta rover: %s", resp);
        close(sock);
        char msg[128];
        snprintf(msg, sizeof(msg), "Caster ha rifiutato la richiesta: %.50s", resp);
        status_ntrip_note_disconnected(msg);
        return -1;
    }

    ESP_LOGI(TAG, "Rover connesso al caster %s:%d mountpoint /%s",
             settings->ntrip_host, settings->ntrip_port, settings->ntrip_mountpoint);
    status_ntrip_note_connected();
    return sock;
}

bool ntrip_rover_client_test_connect(const char *host, uint16_t port, const char *mountpoint,
                                      const char *username, const char *password,
                                      char *out_msg, size_t out_msg_size)
{
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(host, port_str, &hints, &res) != 0 || res == NULL) {
        snprintf(out_msg, out_msg_size, "DNS lookup fallita per %s", host);
        return false;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        snprintf(out_msg, out_msg_size, "Creazione socket fallita");
        return false;
    }

    // Timeout breve: questo e' un test puntuale dalla UI web, non deve
    // lasciare la richiesta HTTP del browser in sospeso a lungo se il
    // caster non risponde affatto.
    struct timeval tv = { .tv_sec = 6, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));

    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        int e = errno;
        close(sock);
        freeaddrinfo(res);
        snprintf(out_msg, out_msg_size, "Connessione a %s:%u fallita (errno %d)", host, port, e);
        return false;
    }
    freeaddrinfo(res);

    char credentials[112];
    int cred_len = snprintf(credentials, sizeof(credentials), "%s:%s", username, password);

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
        mountpoint, (const char *) b64);

    if (send(sock, req, req_len, 0) != req_len) {
        close(sock);
        snprintf(out_msg, out_msg_size, "Invio richiesta fallito");
        return false;
    }

    char resp[128] = {0};
    int r = recv(sock, resp, sizeof(resp) - 1, 0);
    close(sock);
    if (r <= 0) {
        snprintf(out_msg, out_msg_size, "Nessuna risposta dal caster");
        return false;
    }
    resp[r] = '\0';
    if (strncmp(resp, "ICY 200", 7) != 0 && strncmp(resp, "HTTP/1.1 200", 12) != 0) {
        snprintf(out_msg, out_msg_size, "Caster ha rifiutato: %.60s", resp);
        return false;
    }

    snprintf(out_msg, out_msg_size, "Connesso con successo al mountpoint /%s", mountpoint);
    return true;
}

// Isola il prossimo campo separato da ';' in *cursor, terminandolo con '\0'
// - stessa logica di gnss_signal.c/gnss_fix.c, duplicata qui perche' e'
// privata a quel file e il formato del sourcetable NTRIP usa ';' invece
// della ',' delle sentenze NMEA.
static char *next_field_semicolon(char **cursor)
{
    if (!*cursor) {
        return NULL;
    }
    char *start = *cursor;
    char *sep = strchr(start, ';');
    if (sep) {
        *sep = '\0';
        *cursor = sep + 1;
    } else {
        *cursor = NULL;
    }
    return start;
}

size_t ntrip_rover_client_fetch_mountpoints(ntrip_mountpoint_entry_t *out, size_t max_count)
{
    app_settings_t settings = settings_get();

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", settings.ntrip_port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(settings.ntrip_host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGW(TAG, "Sourcetable: DNS lookup fallita per %s", settings.ntrip_host);
        return 0;
    }

    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return 0;
    }

    // Senza questo timeout, un caster che accetta la connessione ma non
    // manda mai il sourcetable (ne' la chiude) blocca qui a oltranza
    // recv() sotto - trovato rivedendo il codice, mai capitato in
    // pratica ma un bug reale: bloccherebbe il task del server web che
    // gestisce questa richiesta, congelando l'intera UI finche' il
    // caster remoto non decide di chiudere da solo. Stesso valore gia'
    // usato in ntrip_rover_client_test_connect() per lo stesso motivo.
    struct timeval tv = { .tv_sec = 8, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof(tv));
    if (connect(sock, res->ai_addr, res->ai_addrlen) != 0) {
        ESP_LOGW(TAG, "Sourcetable: connessione a %s:%d fallita: errno %d", settings.ntrip_host, settings.ntrip_port, errno);
        close(sock);
        freeaddrinfo(res);
        return 0;
    }
    freeaddrinfo(res);

    // Nessuna autenticazione: il sourcetable NTRIP ("GET /" sulla radice,
    // non su una mountpoint specifica) e' sempre pubblico per definizione
    // del protocollo - permette a qualunque client di scoprire cosa offre
    // il caster prima di autenticarsi su una mountpoint scelta.
    const char req[] = "GET / HTTP/1.1\r\nUser-Agent: NTRIP baseesp32/1.0\r\nConnection: close\r\n\r\n";
    if (send(sock, req, sizeof(req) - 1, 0) != (int) sizeof(req) - 1) {
        close(sock);
        return 0;
    }

    // Il sourcetable puo' arrivare in piu' pacchetti TCP - si legge finche'
    // il caster chiude la connessione (Connection: close sopra) o si
    // riempie il buffer, non ci si ferma al primo recv() come nel resto di
    // questo file (li' basta l'intestazione di risposta, qui serve il
    // corpo intero).
    static char buf[4096];
    size_t total = 0;
    while (total < sizeof(buf) - 1) {
        int r = recv(sock, buf + total, sizeof(buf) - 1 - total, 0);
        if (r <= 0) {
            break;
        }
        total += (size_t) r;
    }
    buf[total] = '\0';
    close(sock);

    size_t count = 0;
    char *line = buf;
    while (count < max_count && line && *line) {
        char *newline = strchr(line, '\n');
        if (newline) {
            *newline = '\0';
        }
        char *cr = strchr(line, '\r');
        if (cr) {
            *cr = '\0';
        }
        char *next_line = newline ? newline + 1 : NULL;

        if (strncmp(line, "STR;", 4) != 0) {
            line = next_line;
            continue;
        }
        char *field_cursor = line;
        next_field_semicolon(&field_cursor); // "STR"
        char *name = next_field_semicolon(&field_cursor);
        char *desc = next_field_semicolon(&field_cursor);
        if (name && name[0] != '\0') {
            strncpy(out[count].name, name, sizeof(out[count].name) - 1);
            out[count].name[sizeof(out[count].name) - 1] = '\0';
            if (desc) {
                strncpy(out[count].description, desc, sizeof(out[count].description) - 1);
                out[count].description[sizeof(out[count].description) - 1] = '\0';
            } else {
                out[count].description[0] = '\0';
            }
            count++;
        }
        line = next_line;
    }
    return count;
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
                status_ntrip_note_disconnected("Caster ha chiuso la connessione");
                break;
            } else {
                ESP_LOGW(TAG, "recv fallita, riconnessione: errno %d", errno);
                char msg[128];
                snprintf(msg, sizeof(msg), "Ricezione dati fallita, riconnessione (errno %d)", errno);
                status_ntrip_note_disconnected(msg);
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
    if (send(sock, buf, n, 0) == n) {
        status_note_gga_sent();
    }
}
