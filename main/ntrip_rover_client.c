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
#include "gnss_fix.h"
#include "base_auto.h"
#include "base_check.h"
#include "rtcm3_1005.h"
#include "geo_convert.h"

#include <string.h>
#include <stdlib.h>
#include <errno.h>
#include <stdint.h>
#include <sys/socket.h>
#include <netdb.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_heap_caps.h"
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

// Numero decimale di un campo della riga STR (latitudine o longitudine):
// false se vuoto o con altro dopo il numero.
static bool parse_coord_field(const char *f, double *out)
{
    if (!f) {
        return false;
    }
    char *end = NULL;
    double v = strtod(f, &end);
    if (end == f) {
        return false;
    }
    while (*end == ' ') {
        end++;
    }
    if (*end != '\0') {
        return false;
    }
    *out = v;
    return true;
}

// Solo host e porta del caster, senza copiare tutta la configurazione
// (oltre 2 KB) sullo stack di chi chiama (task del rover o del pannello).
typedef struct {
    char host[sizeof(((app_settings_t *) 0)->ntrip_host)];
    uint16_t port;
} caster_addr_t;

static void peek_caster_addr(const app_settings_t *s, void *ctx)
{
    caster_addr_t *a = ctx;
    strlcpy(a->host, s->ntrip_host, sizeof(a->host));
    a->port = s->ntrip_port;
}

size_t ntrip_rover_client_fetch_mountpoints(ntrip_mountpoint_entry_t *out, size_t max_count)
{
    caster_addr_t caster;
    settings_peek(peek_caster_addr, &caster);

    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", caster.port);

    struct addrinfo hints = {
        .ai_family = AF_INET,
        .ai_socktype = SOCK_STREAM,
    };
    struct addrinfo *res = NULL;
    if (getaddrinfo(caster.host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGW(TAG, "Sourcetable: DNS lookup fallita per %s", caster.host);
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
        ESP_LOGW(TAG, "Sourcetable: connessione a %s:%u fallita: errno %d", caster.host, (unsigned) caster.port, errno);
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
                           caster.host, (unsigned) caster.port);
    if (req_len < 0 || req_len >= (int) sizeof(req) || send(sock, req, req_len, 0) != req_len) {
        net_close_now(sock);
        return 0;
    }

    // Il sourcetable puo' arrivare in piu' pacchetti TCP - si legge finche'
    // il caster chiude la connessione (Connection: close sopra) o si
    // riempie il buffer, non ci si ferma al primo recv() come nel resto di
    // questo file (li' basta l'intestazione di risposta, qui serve il
    // corpo intero).
    // Allocato solo per questa lettura, in PSRAM se c'e' (RAM interna
    // scarsa): 8 KB per stare nelle 32 righe STR di una rete regionale.
    const size_t buf_size = 8192;
    char *buf = heap_caps_malloc(buf_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!buf) {
        buf = malloc(buf_size);
    }
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
        // Nome piu' lungo di rover_mountpoint: troncato porterebbe a un
        // mountpoint che non esiste, si salta.
        if (name && name[0] != '\0' && strlen(name) < sizeof(out[count].name)) {
            strlcpy(out[count].name, name, sizeof(out[count].name));
            if (desc) {
                strncpy(out[count].description, desc, sizeof(out[count].description) - 1);
                out[count].description[sizeof(out[count].description) - 1] = '\0';
            } else {
                out[count].description[0] = '\0';
            }
            // Campi 4-9: formato, dettagli, portante, costellazioni, rete,
            // nazione; poi latitudine e longitudine (gradi decimali).
            for (int k = 0; k < 6; k++) {
                next_field_semicolon(&field_cursor);
            }
            char *lat = next_field_semicolon(&field_cursor);
            char *lon = next_field_semicolon(&field_cursor);
            double la = 0, lo = 0;
            if (parse_coord_field(lat, &la) && parse_coord_field(lon, &lo)) {
                out[count].lat_deg = la;
                out[count].lon_deg = lo;
            } else {
                out[count].lat_deg = 0; // 0/0: "senza coordinate" per base_auto.c
                out[count].lon_deg = 0;
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

// --- Verifica della base (base_check.c) -----------------------------------
// Tipi RTCM arrivati negli ultimi CHECK_RECENT_US (dai contatori di
// rtcm3_stats: un tipo e' "recente" se i suoi byte sono cresciuti) e
// posizione della base dal suo 1005/1006, convertita in lat/lon.
#define CHECK_RECENT_US (60LL * 1000000) // la 1005/1006 arriva di solito ogni 10-30 s
#define CHECK_TYPES_MAX 48

typedef struct {
    uint16_t type;
    uint32_t bytes;
    int64_t seen_us; // 0 = non visto con il collegamento attuale
} check_seen_t;

static SemaphoreHandle_t s_chk_mutex;
static check_seen_t s_seen[CHECK_TYPES_MAX];
static size_t s_seen_n;
static int64_t s_seen_scan_us;
static int64_t s_conn_us; // inizio del collegamento attuale (0 = mai)
static bool s_base_pos_valid;
static double s_base_lat_deg, s_base_lon_deg;
static rtcm3_stat_entry_t s_stat_buf[CHECK_TYPES_MAX]; // solo il task del rover

// Nuovo collegamento (anche verso un'altra base): niente di quanto visto
// prima vale per la base nuova. I byte restano, per riconoscere i nuovi.
static void check_reset(void)
{
    if (!s_chk_mutex) {
        return;
    }
    xSemaphoreTake(s_chk_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_seen_n; i++) {
        s_seen[i].seen_us = 0;
    }
    s_base_pos_valid = false;
    s_conn_us = esp_timer_get_time();
    xSemaphoreGive(s_chk_mutex);
    rtcm3_1005_init();
}

static void check_track(const uint8_t *data, int n)
{
    if (!s_chk_mutex) {
        return;
    }
    rtcm3_position_t pos;
    if (rtcm3_1005_feed(data, (size_t) n, &pos)) {
        double lat, lon, h;
        geo_ecef_to_llh(pos.ecef_x_m, pos.ecef_y_m, pos.ecef_z_m, &lat, &lon, &h);
        if (base_auto_coords_valid(lat, lon)) {
            xSemaphoreTake(s_chk_mutex, portMAX_DELAY);
            s_base_pos_valid = true;
            s_base_lat_deg = lat;
            s_base_lon_deg = lon;
            xSemaphoreGive(s_chk_mutex);
        }
    }
    int64_t now = esp_timer_get_time();
    if (s_seen_scan_us != 0 && now - s_seen_scan_us < 2000000) {
        return; // contatori riletti al massimo ogni 2 s
    }
    s_seen_scan_us = now;
    size_t k = rtcm3_stats_get(s_stat_buf, CHECK_TYPES_MAX);
    xSemaphoreTake(s_chk_mutex, portMAX_DELAY);
    for (size_t i = 0; i < k; i++) {
        size_t j = 0;
        while (j < s_seen_n && s_seen[j].type != s_stat_buf[i].msg_type) {
            j++;
        }
        if (j == s_seen_n) {
            if (s_seen_n == CHECK_TYPES_MAX) {
                continue;
            }
            s_seen[j].type = s_stat_buf[i].msg_type;
            s_seen[j].bytes = 0;
            s_seen[j].seen_us = 0;
            s_seen_n++;
        }
        if (s_stat_buf[i].bytes != s_seen[j].bytes) {
            s_seen[j].bytes = s_stat_buf[i].bytes;
            s_seen[j].seen_us = now;
        }
    }
    xSemaphoreGive(s_chk_mutex);
}

void ntrip_rover_client_base_check(ntrip_rover_base_check_t *out)
{
    memset(out, 0, sizeof(*out));
    out->distance_km = -1;
    if (!s_chk_mutex) {
        out->check = base_check_eval(NULL, 0, -1);
        return;
    }
    out->active = true;
    out->connected = s_sock >= 0;
    int64_t now = esp_timer_get_time();
    uint16_t types[CHECK_TYPES_MAX];
    size_t nt = 0;
    xSemaphoreTake(s_chk_mutex, portMAX_DELAY);
    for (size_t i = 0; i < s_seen_n; i++) {
        if (s_seen[i].seen_us != 0 && now - s_seen[i].seen_us < CHECK_RECENT_US) {
            types[nt++] = s_seen[i].type;
        }
    }
    // Primo minuto di un collegamento: non tutti i messaggi sono ancora
    // arrivati (la 1005/1006 ogni 10-30 s), il giudizio aspetta.
    out->warming = s_conn_us == 0 || now - s_conn_us < CHECK_RECENT_US;
    bool has_pos = s_base_pos_valid;
    double blat = s_base_lat_deg, blon = s_base_lon_deg;
    xSemaphoreGive(s_chk_mutex);

    // Distanza: dalla posizione che la base trasmette (1005/1006), se no
    // dalle coordinate della tabella delle sorgenti (scelta automatica).
    gnss_fix_status_t fix = gnss_fix_get_status();
    bool rover_pos = fix.valid && fix.has_position && base_auto_coords_valid(fix.lat_deg, fix.lon_deg);
    if (has_pos) {
        out->base_pos_from_rtcm = true;
        out->base_lat_deg = blat;
        out->base_lon_deg = blon;
        if (rover_pos) {
            out->distance_km = base_auto_distance_km(fix.lat_deg, fix.lon_deg, blat, blon);
        }
    } else {
        ntrip_rover_auto_status_t ab;
        ntrip_rover_client_auto_status(&ab);
        out->distance_km = ab.distance_km;
    }
    out->check = base_check_eval(types, nt, out->distance_km);
}

// Dati ricevuti dal caster: al ricevitore e ai contatori.
static void feed_rtcm(const uint8_t *data, int n)
{
    gnss_io_write((const char *) data, n);
    rtcm3_stats_feed(data, (size_t) n); // tipi RTCM ricevuti, per il pannello
    check_track(data, n);               // verifica della base
    data_usage_add((uint32_t) n, false);
    status_note_rtcm_bytes((uint32_t) n);
}

// --- Scelta automatica della base piu' vicina (rover_auto_base) -------------
// La decisione e' in base_auto.c (prove sul PC); qui la tabella delle
// sorgenti, la qualita' del fix e il cambio di collegamento. Stato usato
// solo da ntrip_rover_client_task, tranne la copia s_auto_pub (mutex).
typedef struct {
    bool enabled;
    bool first;               // scelta iniziale da fare (avvio o casella appena attivata)
    int64_t last_check_us;    // ultima lettura della tabella (0 = mai)
    int64_t last_rtk_us;      // ultimo istante in RTK fisso/float (o inizio collegamento)
    int64_t last_tick_us;
    uint32_t gen;             // settings_generation() dell'ultima lettura
    char current[33];         // mountpoint da usare
    char fallback[33];        // rover_mountpoint delle impostazioni (base iniziale e ripiego)
    bool have_pos;            // ultima posizione valida del rover
    double lat_deg, lon_deg;
    int quality;              // ultima qualita' della GGA (-1 nessun dato recente)
} rover_auto_t;

static rover_auto_t s_auto = { .first = true, .quality = -1 };

static struct {
    bool enabled;
    bool chosen;
    char mountpoint[33];
    bool has_coords;          // coordinate della base in uso note (dalla tabella)
    double lat_deg, lon_deg;
} s_auto_pub;
static SemaphoreHandle_t s_auto_mutex;

typedef struct {
    bool enabled;
    char mountpoint[33];
} auto_cfg_t;

static void peek_auto_cfg(const app_settings_t *s, void *ctx)
{
    auto_cfg_t *c = ctx;
    c->enabled = s->rover_auto_base;
    strlcpy(c->mountpoint, s->rover_mountpoint, sizeof(c->mountpoint));
}

// Aggiorna la copia per pannello e display. coords: posizione della base in
// uso (NULL = sconosciuta, si tiene quella gia' nota se la base e' la stessa).
static void auto_publish(bool chosen, const base_auto_entry_t *coords)
{
    if (!s_auto_mutex) {
        return;
    }
    xSemaphoreTake(s_auto_mutex, portMAX_DELAY);
    bool same = strcmp(s_auto_pub.mountpoint, s_auto.current) == 0;
    s_auto_pub.enabled = s_auto.enabled;
    s_auto_pub.chosen = chosen;
    strlcpy(s_auto_pub.mountpoint, s_auto.current, sizeof(s_auto_pub.mountpoint));
    if (coords) {
        s_auto_pub.has_coords = true;
        s_auto_pub.lat_deg = coords->lat_deg;
        s_auto_pub.lon_deg = coords->lon_deg;
    } else if (!same) {
        s_auto_pub.has_coords = false;
    }
    xSemaphoreGive(s_auto_mutex);
}

void ntrip_rover_client_auto_status(ntrip_rover_auto_status_t *out)
{
    memset(out, 0, sizeof(*out));
    out->distance_km = -1;
    if (!s_auto_mutex) {
        return;
    }
    xSemaphoreTake(s_auto_mutex, portMAX_DELAY);
    out->enabled = s_auto_pub.enabled;
    out->chosen = s_auto_pub.chosen;
    strlcpy(out->mountpoint, s_auto_pub.mountpoint, sizeof(out->mountpoint));
    bool has = s_auto_pub.has_coords;
    double blat = s_auto_pub.lat_deg;
    double blon = s_auto_pub.lon_deg;
    xSemaphoreGive(s_auto_mutex);
    gnss_fix_status_t fix = gnss_fix_get_status();
    if (has && fix.valid && fix.has_position && base_auto_coords_valid(fix.lat_deg, fix.lon_deg)) {
        out->distance_km = base_auto_distance_km(fix.lat_deg, fix.lon_deg, blat, blon);
    }
}

// Rilegge casella e base iniziale. Non chiude mai il collegamento in corso
// (si perderebbe il fix): casella attivata o base iniziale cambiata fanno
// solo ripetere subito la scelta, che in RTK non cambia base; casella
// spenta: rover_mountpoint al prossimo collegamento.
static void auto_read_settings(void)
{
    auto_cfg_t c;
    settings_peek(peek_auto_cfg, &c);
    s_auto.gen = settings_generation();
    if (c.enabled && !s_auto.enabled) {
        // Casella appena attivata (o avvio): tabella letta subito.
        ESP_LOGI(TAG, "Scelta automatica della base attiva (base iniziale %s)", c.mountpoint);
        s_auto.first = true;
        s_auto.last_check_us = 0;
    }
    if (strcmp(c.mountpoint, s_auto.fallback) != 0) {
        strlcpy(s_auto.fallback, c.mountpoint, sizeof(s_auto.fallback));
        s_auto.first = true;
        s_auto.last_check_us = 0;
    }
    if (s_auto.current[0] == '\0') {
        strlcpy(s_auto.current, c.mountpoint, sizeof(s_auto.current));
    }
    bool was = s_auto.enabled;
    s_auto.enabled = c.enabled;
    if (was != c.enabled) {
        auto_publish(s_auto_pub.chosen, NULL);
    }
}

// Qualita' del fix e ultima posizione valida (al massimo una volta al secondo).
static void auto_sample(int64_t now)
{
    if (s_auto.last_tick_us != 0 && now - s_auto.last_tick_us < 1000000) {
        return;
    }
    s_auto.last_tick_us = now;
    gnss_fix_status_t fix = gnss_fix_get_status();
    // GGA vecchia di oltre 5 s: ricevitore muto, nessun dato.
    int q = (fix.valid && now - fix.last_update_us < 5000000) ? (int) fix.quality : -1;
    s_auto.quality = q;
    if (q == GNSS_FIX_RTK_FIXED || q == GNSS_FIX_RTK_FLOAT) {
        s_auto.last_rtk_us = now;
    }
    if (q >= 1 && q <= 6 && fix.has_position && base_auto_coords_valid(fix.lat_deg, fix.lon_deg)) {
        s_auto.have_pos = true;
        s_auto.lat_deg = fix.lat_deg;
        s_auto.lon_deg = fix.lon_deg;
    }
}

// Legge la tabella e decide, se e' il momento. Ritorna true se il rover
// deve passare a un'altra base (s_auto.current gia' aggiornato).
static bool auto_check(int64_t now)
{
    if (!s_auto.enabled || !s_auto.have_pos) {
        return false; // senza posizione resta la base iniziale
    }
    if (!s_auto.first) {
        if (s_auto.last_check_us != 0 && now - s_auto.last_check_us < (int64_t) BASE_AUTO_CHECK_S * 1000000) {
            return false; // tabella letta da meno di 10 minuti
        }
        // In RTK o senza RTK da poco: niente cambio possibile, inutile
        // scaricare la tabella (traffico sulla SIM).
        if (s_auto.quality == GNSS_FIX_RTK_FIXED || s_auto.quality == GNSS_FIX_RTK_FLOAT ||
            now - s_auto.last_rtk_us < (int64_t) BASE_AUTO_NO_RTK_S * 1000000) {
            return false;
        }
    }
    s_auto.last_check_us = now;
    bool first = s_auto.first;
    s_auto.first = false;

    // Elenchi in PSRAM se c'e' (32 voci, circa 4 KB + 2 KB): RAM interna scarsa.
    const uint32_t caps = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT;
    ntrip_mountpoint_entry_t *mps = heap_caps_calloc(NTRIP_MOUNTPOINTS_MAX, sizeof(*mps), caps);
    if (!mps) {
        mps = calloc(NTRIP_MOUNTPOINTS_MAX, sizeof(*mps));
    }
    base_auto_entry_t *list = heap_caps_calloc(NTRIP_MOUNTPOINTS_MAX, sizeof(*list), caps);
    if (!list) {
        list = calloc(NTRIP_MOUNTPOINTS_MAX, sizeof(*list));
    }
    if (!mps || !list) {
        free(mps);
        free(list);
        ESP_LOGW(TAG, "Base automatica: memoria insufficiente, resta %s", s_auto.current);
        return false;
    }
    size_t n = ntrip_rover_client_fetch_mountpoints(mps, NTRIP_MOUNTPOINTS_MAX);
    for (size_t i = 0; i < n; i++) {
        strlcpy(list[i].name, mps[i].name, sizeof(list[i].name));
        list[i].lat_deg = mps[i].lat_deg;
        list[i].lon_deg = mps[i].lon_deg;
    }
    free(mps);
    if (n == 0) {
        free(list);
        ESP_LOGW(TAG, "Base automatica: tabella delle sorgenti non letta, resta %s", s_auto.current);
        return false;
    }

    uint32_t no_rtk_s = (uint32_t) ((now - s_auto.last_rtk_us) / 1000000);
    base_auto_result_t r = base_auto_choose(list, n, s_auto.have_pos, s_auto.lat_deg, s_auto.lon_deg,
                                            s_auto.current, s_auto.quality, no_rtk_s, first);
    bool change = r.change;
    if (change) {
        if (r.current_km >= 0) {
            ESP_LOGI(TAG, "Base automatica: %s -> %s (%.1f km, prima %.1f km)", s_auto.current, r.mountpoint,
                     r.nearest_km, r.current_km);
        } else {
            ESP_LOGI(TAG, "Base automatica: %s -> %s (%.1f km; base precedente senza coordinate in tabella)",
                     s_auto.current, r.mountpoint, r.nearest_km);
        }
        strlcpy(s_auto.current, r.mountpoint, sizeof(s_auto.current));
        auto_publish(true, &list[r.nearest]);
    } else {
        if (r.nearest >= 0) {
            ESP_LOGI(TAG, "Base automatica: resta %s (%s; piu' vicina %s a %.1f km, %d basi con coordinate)",
                     s_auto.current, base_auto_reason_str(r.reason), list[r.nearest].name, r.nearest_km, r.candidates);
        } else {
            ESP_LOGI(TAG, "Base automatica: resta %s (%s)", s_auto.current, base_auto_reason_str(r.reason));
        }
        auto_publish(r.current >= 0, r.current >= 0 ? &list[r.current] : NULL);
    }
    free(list);
    return change;
}

// Copia delle impostazioni del task: statica, non sullo stack (oltre 2 KB).
static app_settings_t s_task_cfg;

void ntrip_rover_client_task(void *arg)
{
    (void) arg; // porta verso il ricevitore gestita da gnss_io (seriale o I2C)
    uint8_t net_buf[512];

    if (!s_sock_mutex) {
        s_sock_mutex = xSemaphoreCreateMutex();
    }
    if (!s_auto_mutex) {
        s_auto_mutex = xSemaphoreCreateMutex();
    }
    if (!s_chk_mutex) {
        s_chk_mutex = xSemaphoreCreateMutex();
        rtcm3_1005_init();
    }

    while (1) {
        sys_stats_heartbeat(HB_NTRIP);
        net_wait_ready(30000);
        settings_get_into(&s_task_cfg);
        auto_read_settings();
        if (s_auto.enabled) {
            int64_t now = esp_timer_get_time();
            auto_sample(now);
            if (s_auto.first && !s_auto.have_pos) {
                // Nessuna posizione per scegliere: base iniziale (ripiego).
                strlcpy(s_auto.current, s_auto.fallback, sizeof(s_auto.current));
            }
            auto_check(now);
            // Credenziali del rover invariate, cambia solo il mountpoint.
            strlcpy(s_task_cfg.rover_mountpoint, s_auto.current, sizeof(s_task_cfg.rover_mountpoint));
        } else {
            strlcpy(s_auto.current, s_task_cfg.rover_mountpoint, sizeof(s_auto.current));
            auto_publish(false, NULL);
        }
        size_t extra_len = 0;
        int sock = ntrip_rover_connect(&s_task_cfg, net_buf, sizeof(net_buf), &extra_len);
        if (sock < 0) {
            if (s_auto.enabled && strcmp(s_auto.current, s_auto.fallback) != 0) {
                // La base scelta non risponde o rifiuta: si torna alla base
                // iniziale; la tabella si riguarda al prossimo controllo.
                ESP_LOGW(TAG, "Base automatica: collegamento a %s fallito, ripiego su %s", s_auto.current, s_auto.fallback);
                strlcpy(s_auto.current, s_auto.fallback, sizeof(s_auto.current));
                auto_publish(false, NULL);
            }
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
        // Il conto "senza RTK" riparte dal collegamento: la base nuova ha
        // 30 s per dare il fix prima di un altro cambio.
        s_auto.last_rtk_us = esp_timer_get_time();
        check_reset(); // base nuova: tipi e posizione da rileggere
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
            // Scelta automatica della base: controllo leggero a ogni giro
            // (qualita' al massimo una volta al secondo, tabella al massimo
            // ogni 10 minuti e solo fuori dall'RTK). Impostazioni rilette
            // solo quando cambiano (casella attivata: controllo subito).
            int64_t now = esp_timer_get_time();
            if (settings_generation() != s_auto.gen) {
                auto_read_settings();
            }
            bool switch_base = false;
            if (s_auto.enabled) {
                auto_sample(now);
                switch_base = auto_check(now);
            }
            if (switch_base) {
                char msg[96];
                snprintf(msg, sizeof(msg), "Cambio base: collegamento a %.40s", s_auto.current);
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
