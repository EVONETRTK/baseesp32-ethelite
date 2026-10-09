#include "ntrip_rover_client.h"
#include "gnss_io.h"
#include "settings.h"
#include "status.h"
#include "net_util.h"
#include "sys_stats.h"
#include "esp_timer.h"
#include "rtcm3_stats.h"
#include "data_usage.h"
#include "ntrip_reply.h"

#include <string.h>
#include <stdlib.h>
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

// Timeout dell'handshake (invio della richiesta e attesa della risposta).
// Prima la recv() della risposta non aveva limite: con un caster che accetta
// la connessione ma non risponde, o una linea cellulare caduta subito dopo
// il GET, il task restava fermo finche' il controllo dei task bloccati non
// riavviava tutto il dispositivo (5 minuti).
#define HANDSHAKE_TIMEOUT_S 10

// Legge la risposta del caster fino alla fine dell'intestazione e lascia in
// extra/extra_len i byte di dati gia' arrivati dietro di essa (prima si
// buttavano, e con una risposta HTTP lunga il resto dell'intestazione finiva
// nel ricevitore GNSS). resp viene terminata alla fine dell'intestazione.
// Ritorna: >0 risposta letta, 0 nessuna risposta, -1 risposta 200 con
// trasferimento "a blocchi" (chunked, non gestito: le dimensioni dei
// blocchi finirebbero nel ricevitore come dati).
static int read_reply_header(int sock, char *resp, size_t resp_size, uint8_t *extra, size_t extra_size, size_t *extra_len)
{
    *extra_len = 0;
    size_t total = 0;
    resp[0] = '\0';
    while (total < resp_size - 1) {
        int r = recv(sock, resp + total, resp_size - 1 - total, 0);
        if (r <= 0) {
            break;
        }
        total += (size_t) r;
        resp[total] = '\0';
        // NTRIP 1.0: "ICY 200 OK\r\n" e subito i dati (a volte una riga vuota
        // in piu'); per le altre risposte (HTTP, errori) si aspetta la riga
        // vuota che chiude l'intestazione.
        if (strncmp(resp, "ICY 200", 7) == 0 && strstr(resp, "\r\n")) {
            break;
        }
        if (strstr(resp, "\r\n\r\n")) {
            break;
        }
    }
    if (total == 0) {
        return 0;
    }
    const char *body = NULL;
    if (strncmp(resp, "ICY 200", 7) == 0) {
        const char *eol = strstr(resp, "\r\n");
        if (eol) {
            body = eol + 2;
            if ((size_t) (body - resp) + 2 <= total && body[0] == '\r' && body[1] == '\n') {
                body += 2;
            }
        }
    } else {
        const char *eoh = strstr(resp, "\r\n\r\n");
        if (eoh) {
            body = eoh + 4;
            if (strncmp(resp, "HTTP/1.", 7) == 0 && strncmp(resp + 8, " 200", 4) == 0) {
                char saved = *body;
                resp[body - resp] = '\0'; // solo l'intestazione per la ricerca
                bool chunked = strcasestr(resp, "Transfer-Encoding: chunked") != NULL;
                resp[body - resp] = saved;
                if (chunked) {
                    return -1;
                }
            }
        }
    }
    if (body) {
        // I dati binari possono contenere '\0': si conta su total, non su strlen.
        size_t hdr = (size_t) (body - resp);
        size_t n = total > hdr ? total - hdr : 0;
        if (n > extra_size) {
            n = extra_size;
        }
        memcpy(extra, body, n);
        *extra_len = n;
        resp[hdr] = '\0';
    }
    return (int) total;
}

// Richiesta GET della mountpoint. "Host" e' obbligatorio in HTTP/1.1: i
// caster dietro un proxy (nginx e simili) senza rispondevano 400.
static int build_get_request(char *req, size_t req_size, const char *host, uint16_t port, const char *mountpoint,
                             const char *username, const char *password)
{
    char credentials[112];
    int cred_len = snprintf(credentials, sizeof(credentials), "%s:%s", username, password);
    if (cred_len < 0) {
        cred_len = 0;
    } else if (cred_len >= (int) sizeof(credentials)) {
        cred_len = (int) sizeof(credentials) - 1;
    }

    unsigned char b64[160];
    size_t b64_len = 0;
    mbedtls_base64_encode(b64, sizeof(b64) - 1, &b64_len, (const unsigned char *) credentials, (size_t) cred_len);
    b64[b64_len] = '\0';
    memset(credentials, 0, sizeof(credentials));

    int req_len = snprintf(req, req_size,
        "GET /%s HTTP/1.1\r\n"
        "Host: %s:%u\r\n"
        "User-Agent: NTRIP baseesp32/1.0\r\n"
        "Authorization: Basic %s\r\n"
        "Connection: close\r\n"
        "\r\n",
        mountpoint, host, (unsigned) port, (const char *) b64);
    if (req_len < 0 || req_len >= (int) req_size) {
        return -1;
    }
    return req_len;
}

static bool reply_is_ok(const char *resp)
{
    return strncmp(resp, "ICY 200", 7) == 0 || strncmp(resp, "HTTP/1.1 200", 12) == 0 ||
           strncmp(resp, "HTTP/1.0 200", 12) == 0;
}

static void apply_data_socket_options(int sock)
{
    // Timeout di ricezione sul socket dati: senza, se la connessione si
    // interrompe in modo silenzioso (es. un blip di rete che lascia il
    // socket "a zombie" senza che arrivi mai un FIN/RST fino al prossimo
    // invio nostro, che qui non c'e' perche' questo socket riceve soltanto),
    // la recv() nel ciclo sotto resta bloccata per sempre: nessun errore,
    // nessuno 0, quindi il ciclo di riconnessione piu' in basso non scatta
    // mai. Causa reale osservata in campo: un rover restato scollegato per
    // ore mentre un altro sulla stessa rete si riconnetteva da solo -
    // l'unica differenza era che quell'altro client rifaceva comunque una
    // scrittura periodica (il GGA) che avrebbe fatto emergere l'errore.
    // Un timeout largo (il flusso RTCM arriva tipicamente almeno ogni
    // secondo) fa si' che una recv() senza dati per troppo tempo torni con
    // errore invece di restare appesa, e il ciclo sotto la tratta come
    // qualunque altro errore: chiude e riprova.
    struct timeval rcv_tv = { .tv_sec = 20, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &rcv_tv, sizeof(rcv_tv));
    // Invio del GGA (dal task che legge l'NMEA): al massimo 2 s, per non
    // fermare la lettura del ricevitore se la linea e' bloccata.
    struct timeval snd_tv = { .tv_sec = 2, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &snd_tv, sizeof(snd_tv));
}

// Handshake NTRIP client "GET": richiede la mountpoint con autenticazione
// Basic. Accetta sia la risposta NTRIP 1.0 ("ICY 200 OK") sia quella
// NTRIP 2.0/HTTP ("HTTP/1.1 200 OK") - va verificato quale usa realmente
// il caster EVONETRTK. I dati RTCM arrivati insieme alla risposta finiscono
// in extra (da girare al ricevitore come il resto del flusso).
static int ntrip_rover_connect(const app_settings_t *settings, uint8_t *extra, size_t extra_size, size_t *extra_len)
{
    *extra_len = 0;
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

    struct timeval hs_tv = { .tv_sec = HANDSHAKE_TIMEOUT_S, .tv_usec = 0 };
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, &hs_tv, sizeof(hs_tv));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, &hs_tv, sizeof(hs_tv));

    if (net_connect_timeout(sock, res->ai_addr, res->ai_addrlen, 8) != 0) {
        int e = errno;
        ESP_LOGE(TAG, "Connessione a %s:%d fallita: errno %d", settings->ntrip_host, settings->ntrip_port, e);
        net_close_now(sock);
        freeaddrinfo(res);
        char msg[160];
        const char *why = net_errno_text(e);
        if (why) {
            snprintf(msg, sizeof(msg), "Connessione al caster fallita: %s", why);
        } else {
            snprintf(msg, sizeof(msg), "Connessione al caster fallita (errno %d)", e);
        }
        status_ntrip_note_disconnected(msg);
        return -1;
    }
    freeaddrinfo(res);

    char req[384];
    int req_len = build_get_request(req, sizeof(req), settings->ntrip_host, settings->ntrip_port,
                                    settings->rover_mountpoint, settings->rover_username, settings->rover_password);
    if (req_len < 0 || send(sock, req, req_len, 0) != req_len) {
        ESP_LOGE(TAG, "Invio richiesta NTRIP GET fallito: errno %d", errno);
        memset(req, 0, sizeof(req));
        net_close_now(sock);
        status_ntrip_note_disconnected("Invio richiesta fallito");
        return -1;
    }
    memset(req, 0, sizeof(req)); // contiene le credenziali

    char resp[512];
    int r = read_reply_header(sock, resp, sizeof(resp), extra, extra_size, extra_len);
    if (r == 0) {
        ESP_LOGE(TAG, "Nessuna risposta dal caster");
        net_close_now(sock);
        status_ntrip_note_disconnected("Nessuna risposta dal caster");
        return -1;
    }
    if (r < 0) {
        net_close_now(sock);
        const char *msg = "Caster ha risposto in NTRIP 2.0 \"a blocchi\" (chunked), non gestito dal rover";
        ESP_LOGE(TAG, "%s", msg);
        status_ntrip_note_disconnected(msg);
        return -1;
    }
    if (!reply_is_ok(resp)) {
        net_close_now(sock);
        char *eol = strpbrk(resp, "\r\n");
        if (eol) {
            *eol = '\0'; // solo la prima riga della risposta
        }
        const char *why = ntrip_explain_reply(resp, false);
        char msg[200];
        snprintf(msg, sizeof(msg), "Caster ha rifiutato: %.150s", why ? why : resp);
        ESP_LOGE(TAG, "%s", msg);
        status_ntrip_note_disconnected(msg);
        return -1;
    }

    ESP_LOGI(TAG, "Rover connesso al caster %s:%d mountpoint /%s",
             settings->ntrip_host, settings->ntrip_port, settings->rover_mountpoint);
    status_ntrip_note_connected();

    apply_data_socket_options(sock);
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

    if (net_connect_timeout(sock, res->ai_addr, res->ai_addrlen, 8) != 0) {
        int e = errno;
        net_close_now(sock);
        freeaddrinfo(res);
        const char *why = net_errno_text(e);
        if (why) {
            snprintf(out_msg, out_msg_size, "Connessione a %s:%u fallita: %s", host, port, why);
        } else {
            snprintf(out_msg, out_msg_size, "Connessione a %s:%u fallita (errno %d)", host, port, e);
        }
        return false;
    }
    freeaddrinfo(res);

    char req[384];
    int req_len = build_get_request(req, sizeof(req), host, port, mountpoint, username, password);
    if (req_len < 0 || send(sock, req, req_len, 0) != req_len) {
        memset(req, 0, sizeof(req));
        net_close_now(sock);
        snprintf(out_msg, out_msg_size, "Invio richiesta fallito");
        return false;
    }
    memset(req, 0, sizeof(req));

    char resp[128] = {0};
    int r = recv(sock, resp, sizeof(resp) - 1, 0);
    net_close_now(sock);
    if (r <= 0) {
        snprintf(out_msg, out_msg_size, "Nessuna risposta dal caster");
        return false;
    }
    resp[r] = '\0';
    if (!reply_is_ok(resp)) {
        char *eol = strpbrk(resp, "\r\n");
        if (eol) {
            *eol = '\0'; // solo la prima riga della risposta
        }
        const char *why = ntrip_explain_reply(resp, false);
        snprintf(out_msg, out_msg_size, "Caster ha rifiutato: %.100s", why ? why : resp);
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
    if (net_connect_timeout(sock, res->ai_addr, res->ai_addrlen, 8) != 0) {
        ESP_LOGW(TAG, "Sourcetable: connessione a %s:%d fallita: errno %d", settings.ntrip_host, settings.ntrip_port, errno);
        net_close_now(sock);
        freeaddrinfo(res);
        return 0;
    }
    freeaddrinfo(res);

    // Nessuna autenticazione: il sourcetable NTRIP ("GET /" sulla radice,
    // non su una mountpoint specifica) e' sempre pubblico per definizione
    // del protocollo - permette a qualunque client di scoprire cosa offre
    // il caster prima di autenticarsi su una mountpoint scelta.
    char req[160];
    int req_len = snprintf(req, sizeof(req),
                           "GET / HTTP/1.1\r\nHost: %s:%u\r\nUser-Agent: NTRIP baseesp32/1.0\r\nConnection: close\r\n\r\n",
                           settings.ntrip_host, (unsigned) settings.ntrip_port);
    if (req_len < 0 || req_len >= (int) sizeof(req) || send(sock, req, req_len, 0) != req_len) {
        net_close_now(sock);
        return 0;
    }

    // Il sourcetable puo' arrivare in piu' pacchetti TCP - si legge finche'
    // il caster chiude la connessione (Connection: close sopra) o si
    // riempie il buffer, non ci si ferma al primo recv() come nel resto di
    // questo file (li' basta l'intestazione di risposta, qui serve il
    // corpo intero).
    // Allocato solo per questa lettura (prima 4 KB fissi in RAM).
    const size_t buf_size = 4096;
    char *buf = malloc(buf_size);
    if (!buf) {
        net_close_now(sock);
        return 0;
    }
    size_t total = 0;
    while (total < buf_size - 1) {
        int r = recv(sock, buf + total, buf_size - 1 - total, 0);
        if (r <= 0) {
            break;
        }
        total += (size_t) r;
    }
    buf[total] = '\0';
    net_close_now(sock);

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
    free(buf);
    return count;
}

#define GGA_INTERVAL_US (10LL * 1000000)
static int64_t s_last_gga_us; // protetto da s_sock_mutex
static uint32_t s_retry_ms = 5000;

static void set_active_sock(int sock)
{
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    s_last_gga_us = 0; // nuovo collegamento: il primo GGA parte subito
    s_sock = sock;
    xSemaphoreGive(s_sock_mutex);
}

// Dati ricevuti dal caster: al ricevitore e ai contatori.
static void feed_rtcm(const uint8_t *data, int n)
{
    gnss_io_write((const char *) data, n);
    rtcm3_stats_feed(data, (size_t) n); // tipi RTCM ricevuti, per il pannello
    data_usage_add((uint32_t) n, false);
    status_note_rtcm_bytes((uint32_t) n);
}

void ntrip_rover_client_task(void *arg)
{
    (void) arg; // porta verso il ricevitore gestita da gnss_io (seriale o I2C)
    uint8_t net_buf[512];

    if (!s_sock_mutex) {
        s_sock_mutex = xSemaphoreCreateMutex();
    }

    while (1) {
        sys_stats_heartbeat(HB_NTRIP);
        net_wait_ready(30000);
        app_settings_t settings = settings_get();
        size_t extra_len = 0;
        int sock = ntrip_rover_connect(&settings, net_buf, sizeof(net_buf), &extra_len);
        if (sock < 0) {
            // Attesa crescente (5, 10, 20... fino a 120 s): con password o
            // mountpoint sbagliati il rover riprovava ogni 5 s all'infinito,
            // 12 volte al minuto, e un caster vero puo' bloccare indirizzo o
            // account (collaudo del 05/10/2026). Si azzera al primo successo.
            // Impostazioni corrette dal pannello durante l'attesa: si riprova
            // subito (prima si aspettava fino a 2 minuti anche dopo aver
            // rimesso la password giusta, collaudo del 06/10/2026).
            uint32_t gen = settings_generation();
            bool changed = false;
            for (uint32_t waited = 0; waited < s_retry_ms && !changed; waited += 1000) {
                vTaskDelay(pdMS_TO_TICKS(1000));
                sys_stats_heartbeat(HB_NTRIP);
                changed = settings_generation() != gen;
            }
            s_retry_ms = changed ? 5000 : (s_retry_ms * 2 > 120000 ? 120000 : s_retry_ms * 2);
            continue;
        }
        s_retry_ms = 5000;
        if (extra_len > 0) {
            feed_rtcm(net_buf, (int) extra_len); // RTCM arrivato insieme alla risposta
        }

        set_active_sock(sock);

        while (1) {
            sys_stats_heartbeat(HB_NTRIP);
            int n = recv(sock, net_buf, sizeof(net_buf), 0);
            if (n > 0) {
                feed_rtcm(net_buf, n);
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

        // Prima si toglie il socket a ntrip_rover_client_forward_gga() (che
        // invia tenendo il mutex), poi lo si chiude: cosi' un GGA non puo'
        // partire su un numero di socket gia' chiuso e magari riusato da
        // un'altra connessione (es. una richiesta del pannello).
        set_active_sock(-1);
        net_close_now(sock);
        // 2s era troppo poco: il caster rileva una connessione precedente
        // interrotta bruscamente solo dopo il proprio keepalive TCP (decine
        // di secondi), e nel frattempo tiene occupato lo slot di sessione
        // dell'account (limite anti-condivisione password). Riprovare troppo
        // in fretta colpiva sempre quella finestra, venendo rifiutati per
        // "troppe sessioni contemporanee" - osservato in pratica.
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
}

void ntrip_rover_client_forward_gga(const char *line, size_t len)
{
    if (!s_sock_mutex || len > 100) {
        return;
    }

    // Invio tenendo il mutex (il socket ha un timeout di invio di 2 s): il
    // task del rover non puo' chiuderlo nel frattempo.
    xSemaphoreTake(s_sock_mutex, portMAX_DELAY);
    int sock = s_sock;
    // Ogni 10 s (il primo subito dopo il collegamento): ai caster basta
    // cosi' per scegliere la stazione o calcolare la VRS. Ogni secondo erano
    // circa 7 MB al giorno di traffico in piu' sulla SIM (collaudo del 05/10).
    int64_t now = esp_timer_get_time();
    if (sock < 0 || (s_last_gga_us != 0 && now - s_last_gga_us < GGA_INTERVAL_US)) {
        xSemaphoreGive(s_sock_mutex);
        return;
    }
    s_last_gga_us = now;

    char buf[128];
    int n = snprintf(buf, sizeof(buf), "%.*s\r\n", (int) len, line);
    bool ok = send(sock, buf, n, 0) == n;
    xSemaphoreGive(s_sock_mutex);
    if (ok) {
        data_usage_add((uint32_t) n, true);
        status_note_gga_sent();
    }
}
