// Fuzzing della lettura delle risposte del caster NTRIP:
//   main/ntrip_rover_client.c: read_reply_header (intestazione, dati RTCM
//     arrivati insieme, "chunked"), prova di connessione del pannello,
//     tabella delle sorgenti (STR;...), GGA verso il caster;
//   main/ntrip_client.c (base): risposta all'handshake SOURCE;
//   main/ntrip_reply.h: spiegazione dei rifiuti.
// I socket sono finti (stub in fuzz/stub/sys/socket.h): recv() restituisce
// la risposta presa dall'input a pezzi di misura casuale.
// Input: [opzioni][risposta del caster]. Opzioni: bit 0-2 funzione provata,
// bit 3 campi delle impostazioni lunghi al massimo, bit 4 recv a pezzi da
// 1..8 byte, bit 5 send che fallisce.
// SORGENTI: ntrip_client.c rtcm3_stats.c
#include "motore.h"
#include "aiuti.h"
#include "../../../main/ntrip_rover_client.c"

#include "ntrip_client.h"

// --- rete finta ---------------------------------------------------------------------
static const uint8_t *g_risp;
static size_t g_risp_n, g_risp_i;
static uint8_t g_opz;
static int g_aperti;

int socket(int domain, int type, int protocol) { (void) domain; (void) type; (void) protocol; g_aperti++; return 7; }
int connect(int s, const struct sockaddr *addr, socklen_t len) { (void) s; (void) addr; (void) len; return 0; }
int setsockopt(int s, int level, int name, const void *val, socklen_t len) { (void) s; (void) level; (void) name; (void) val; (void) len; return 0; }
int getsockopt(int s, int level, int name, void *val, socklen_t *len)
{
    (void) s; (void) level; (void) name; (void) len;
    *(int *) val = 0;
    return 0;
}
int select(int n, fd_set *r, fd_set *w, fd_set *e, struct timeval *tv) { (void) n; (void) r; (void) w; (void) e; (void) tv; return 1; }
int fcntl(int fd, int cmd, ...) { (void) fd; (void) cmd; return 0; }
int fuzz_socket_close(int fd) { (void) fd; g_aperti--; return 0; }
int send(int s, const void *data, size_t len, int flags)
{
    (void) s; (void) flags;
    volatile uint8_t x = 0;
    for (size_t i = 0; i < len; i++) x ^= ((const uint8_t *) data)[i]; // il buffer deve essere leggibile tutto
    return (g_opz & 32) ? -1 : (int) len;
}
int recv(int s, void *mem, size_t len, int flags)
{
    (void) s; (void) flags;
    if (g_risp_i >= g_risp_n) return 0;
    size_t k = (g_opz & 16) ? 1 + fuzz_caso() % 8 : 1 + fuzz_caso() % 600;
    if (k > len) k = len;
    if (k > g_risp_n - g_risp_i) k = g_risp_n - g_risp_i;
    // il firmware deve passare un buffer grande almeno len: si scrive tutto
    memcpy(mem, g_risp + g_risp_i, k);
    g_risp_i += k;
    return (int) k;
}
static struct sockaddr g_addr;
static struct addrinfo g_ai = { 0, AF_INET, SOCK_STREAM, 0, sizeof(struct sockaddr), &g_addr, NULL, NULL };
int getaddrinfo(const char *host, const char *port, const struct addrinfo *hints, struct addrinfo **res)
{
    (void) host; (void) port; (void) hints;
    *res = &g_ai;
    return 0;
}
void freeaddrinfo(struct addrinfo *res) { (void) res; }

int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen)
{
    static const char t[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t need = 4 * ((slen + 2) / 3);
    if (dlen < need + 1) {
        *olen = need + 1;
        return MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL;
    }
    size_t k = 0;
    for (size_t i = 0; i < slen; i += 3) {
        uint32_t v = (uint32_t) src[i] << 16 | (i + 1 < slen ? (uint32_t) src[i + 1] << 8 : 0) | (i + 2 < slen ? src[i + 2] : 0);
        dst[k++] = (unsigned char) t[v >> 18];
        dst[k++] = (unsigned char) t[(v >> 12) & 63];
        dst[k++] = (unsigned char) (i + 1 < slen ? t[(v >> 6) & 63] : '=');
        dst[k++] = (unsigned char) (i + 2 < slen ? t[v & 63] : '=');
    }
    dst[k] = 0;
    *olen = k;
    return 0;
}

// --- altri sostituti --------------------------------------------------------------------
static app_settings_t g_set;
app_settings_t settings_get(void) { return g_set; }
uint32_t settings_generation(void) { return 1; }
void status_ntrip_note_connected(void) {}
void status_ntrip_note_disconnected(const char *reason) { FUZZ_VERIFICA(strlen(reason) < 300); }
net_status_t status_get_net(void) { return NET_STATUS_NONE + 1; }
bool eth_link_is_connected(void) { return true; }
void status_note_rtcm_bytes(uint32_t n) { (void) n; }
void status_note_gga_sent(void) {}
void sys_stats_heartbeat(heartbeat_t which) { (void) which; }
void data_usage_add(uint32_t bytes, bool sent) { (void) bytes; (void) sent; }
int gnss_io_write(const void *data, size_t len) { (void) data; return (int) len; }
size_t xStreamBufferReceive(StreamBufferHandle_t s, void *buf, size_t len, TickType_t wait) { (void) s; (void) buf; (void) len; (void) wait; return 0; }
TickType_t xTaskGetTickCount(void) { return 0; }

static void impostazioni(bool lunghe)
{
    memset(&g_set, 0, sizeof(g_set));
    if (lunghe) {
        memset(g_set.ntrip_host, 'h', sizeof(g_set.ntrip_host) - 1);
        memset(g_set.rover_mountpoint, 'M', sizeof(g_set.rover_mountpoint) - 1);
        memset(g_set.rover_username, 'u', sizeof(g_set.rover_username) - 1);
        memset(g_set.rover_password, 'p', sizeof(g_set.rover_password) - 1);
        memset(g_set.ntrip_mountpoint, 'B', sizeof(g_set.ntrip_mountpoint) - 1);
        memset(g_set.ntrip_password, 'q', sizeof(g_set.ntrip_password) - 1);
        g_set.ntrip_port = 65535;
    } else {
        strlcpy(g_set.ntrip_host, "caster.esempio.it", sizeof(g_set.ntrip_host));
        strlcpy(g_set.rover_mountpoint, "PROVA", sizeof(g_set.rover_mountpoint));
        strlcpy(g_set.rover_username, "utente", sizeof(g_set.rover_username));
        strlcpy(g_set.rover_password, "segreta", sizeof(g_set.rover_password));
        strlcpy(g_set.ntrip_mountpoint, "BASE", sizeof(g_set.ntrip_mountpoint));
        strlcpy(g_set.ntrip_password, "segreta", sizeof(g_set.ntrip_password));
        g_set.ntrip_port = 2101;
    }
}

// --- bersaglio --------------------------------------------------------------------------
void bersaglio_avvio(void)
{
    rtcm3_stats_init();
    s_sock_mutex = xSemaphoreCreateMutex();
    fuzz_imposta_max_len(5000);
}

static void seme(uint8_t opz, const char *r)
{
    static uint8_t b[5000];
    size_t n = strlen(r);
    b[0] = opz;
    memcpy(b + 1, r, n);
    fuzz_seme(b, n + 1);
}

void bersaglio_semi(void)
{
    static const char *const risposte[] = {
        "ICY 200 OK\r\n\xD3\x00\x13\x3E\xD7\xD3\x03\x8A\xC5\x4F\x99\xD2\x03\x30\x95\x7F\x2E\x09\xB0\xBE\xF0\x34\xF6\x3D\x42",
        "ICY 200 OK\r\n\r\n",
        "HTTP/1.1 200 OK\r\nNtrip-Version: Ntrip/2.0\r\nContent-Type: gnss/data\r\n\r\n\xD3\x00\x00\x47\xEA\x4B",
        "HTTP/1.1 200 OK\r\nTransfer-Encoding: chunked\r\n\r\n1a\r\n",
        "HTTP/1.0 401 Unauthorized\r\nWWW-Authenticate: Basic realm=\"/PROVA\"\r\n\r\n",
        "SOURCETABLE 200 OK\r\nServer: NTRIP Caster\r\nContent-Type: text/plain\r\n\r\n"
        "STR;PROVA;Agricoltura standard (MSM4);RTCM 3.2;1005(10),1077(1);2;GPS+GLO+GAL+BDS;EVONETRTK;ITA;41.00;16.50;1;0;sNTRIP;none;B;N;9600;\r\n"
        "STR;AGRI_MSM7;Agricoltura (MSM7);RTCM 3.3;1006(10);2;GPS;NET;ITA;41.10;16.60;1;0;x;none;B;N;0;\r\n"
        "CAS;caster.esempio.it;2101;ESEMPIO;Ente;0;ITA;41.0;16.5;\r\nENDSOURCETABLE\r\n",
        "ERROR - Bad Password\r\n",
        "HTTP/1.1 409 Conflict\r\n\r\nmountpoint in uso",
        "OK\r\n",
    };
    for (size_t i = 0; i < sizeof(risposte) / sizeof(risposte[0]); i++) {
        for (uint8_t f = 0; f < 6; f++) seme(f, risposte[i]);
    }
    // GGA verso il caster
    seme(6, "$GNGGA,101010.00,4100.0000000,N,01630.0000000,E,1,10,0.9,409.5000,M,40.500,M,,*43");
    static const char *const parole[] = { "ICY 200 OK", "HTTP/1.1 200", "HTTP/1.0 ", "\r\n", "\r\n\r\n", "STR;", ";",
                                          "Transfer-Encoding: chunked", "SOURCETABLE", " 404", "401", "409", "402", "429" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    g_opz = d[0];
    g_risp = d + 1;
    g_risp_n = n - 1;
    g_risp_i = 0;
    g_aperti = 0;
    impostazioni((g_opz & 8) != 0);
    char msg_cap = (char) (1 + fuzz_caso() % 200);
    size_t cap = (size_t) (unsigned char) msg_cap;
    char *msg = (char *) fuzz_buffer(cap);
    switch (g_opz & 7) {
    case 0: { // connessione del rover (intestazione + dati gia' arrivati)
        uint8_t *extra = fuzz_buffer(512);
        size_t extra_len = 0;
        int s = ntrip_rover_connect(&g_set, extra, 512, &extra_len);
        FUZZ_VERIFICA(extra_len <= 512);
        if (s >= 0 && extra_len) feed_rtcm(extra, (int) extra_len);
        break;
    }
    case 1: // prova di connessione dal pannello
        (void) ntrip_rover_client_test_connect(g_set.ntrip_host, g_set.ntrip_port, g_set.rover_mountpoint,
                                               g_set.rover_username, g_set.rover_password, msg, cap);
        FUZZ_VERIFICA(strlen(msg) < cap);
        break;
    case 2: { // tabella delle sorgenti
        size_t max = fuzz_caso() % (NTRIP_MOUNTPOINTS_MAX + 1);
        ntrip_mountpoint_entry_t *out = (ntrip_mountpoint_entry_t *) fuzz_buffer(max * sizeof(ntrip_mountpoint_entry_t));
        size_t got = ntrip_rover_client_fetch_mountpoints(out, max);
        FUZZ_VERIFICA(got <= max);
        for (size_t i = 0; i < got; i++) {
            FUZZ_VERIFICA(strlen(out[i].name) < sizeof(out[i].name));
            FUZZ_VERIFICA(strlen(out[i].description) < sizeof(out[i].description));
        }
        break;
    }
    case 3: // base: handshake SOURCE
        (void) ntrip_client_test_source(g_set.ntrip_host, g_set.ntrip_port, g_set.ntrip_mountpoint, g_set.ntrip_password,
                                        msg, cap);
        FUZZ_VERIFICA(strlen(msg) < cap);
        break;
    case 4: // spiegazione dei rifiuti
        (void) ntrip_explain_reply(fuzz_stringa(d + 1, n - 1), (g_opz & 8) != 0);
        break;
    case 5: { // intestazione letta in un buffer piccolo
        size_t rs = 2 + fuzz_caso() % 64, es = fuzz_caso() % 32;
        char *resp = (char *) fuzz_buffer(rs);
        uint8_t *extra = fuzz_buffer(es);
        size_t extra_len = 0;
        int r = read_reply_header(7, resp, rs, extra, es, &extra_len);
        FUZZ_VERIFICA(extra_len <= es);
        if (r != 0) FUZZ_VERIFICA(strlen(resp) < rs);
        break;
    }
    default: { // GGA verso il caster
        s_sock = 7;
        s_last_gga_us = 0;
        size_t len = n - 1;
        char *line = fuzz_stringa(d + 1, len);
        ntrip_rover_client_forward_gga(line, strlen(line));
        s_sock = -1;
        break;
    }
    }
}
