#include "vpn_link.h"
#include "license.h"
#include "settings.h"
#include "status.h"
#include "time_sync.h"
#include "eth_link.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_random.h"
#include "esp_wireguard.h"
#include "mbedtls/base64.h"
#include "lwip/netif.h"

static const char *TAG = "vpn";

// Dal componente esp_wireguard (src/crypto/refc/x25519.h): moltiplicazione
// sulla curva 25519, usata per ricavare la chiave pubblica dalla privata.
extern int x25519(unsigned char out[32], const unsigned char scalar[32], const unsigned char base[32], int clamp);

#define VPN_DEFAULT_PORT       51820
#define VPN_DEFAULT_KEEPALIVE  25
#define VPN_RETRY_US           (30LL * 1000000)       // nuovo tentativo dopo un errore
#define VPN_DOWN_RESTART_US    (5LL * 60 * 1000000)   // tunnel giu' da 5 min: si ricrea

// Configurazione copiata dalle impostazioni: wireguard_config_t tiene solo
// puntatori, le stringhe devono restare valide finche' il tunnel e' attivo.
typedef struct {
    bool enable;
    char priv[48];
    char addr[16];
    char peer[48];
    char endpoint[64];
    uint16_t port;
    uint16_t keepalive;
} vpn_cfg_t;

static vpn_cfg_t s_cfg;          // in uso dal tunnel attivo
static wireguard_config_t s_wg_cfg;
static wireguard_ctx_t s_ctx;
static bool s_platform_ok;
static bool s_started;
static bool s_up;
static int s_started_net = -1;   // rete su cui e' stato creato il tunnel
static int64_t s_started_us, s_up_since_us, s_last_up_us, s_retry_after_us;
static char s_pub[48];
static char s_last_error[64];

static void set_error(const char *msg)
{
    strlcpy(s_last_error, msg, sizeof(s_last_error));
}

static void peek_cfg(const app_settings_t *s, void *ctx)
{
    vpn_cfg_t *c = (vpn_cfg_t *) ctx;
    c->enable = s->vpn_enable && license_has(LIC_BASE);
    strlcpy(c->priv, s->vpn_private_key, sizeof(c->priv));
    strlcpy(c->addr, s->vpn_address, sizeof(c->addr));
    strlcpy(c->peer, s->vpn_peer_public_key, sizeof(c->peer));
    strlcpy(c->endpoint, s->vpn_endpoint, sizeof(c->endpoint));
    c->port = s->vpn_port ? s->vpn_port : VPN_DEFAULT_PORT;
    c->keepalive = s->vpn_keepalive_s ? s->vpn_keepalive_s : VPN_DEFAULT_KEEPALIVE;
}

static bool public_from_private(const char *priv_b64, char *pub_b64, size_t pub_size)
{
    unsigned char priv[32], pub[32];
    static const unsigned char basepoint[32] = { 9 };
    size_t n = 0;
    if (mbedtls_base64_decode(priv, sizeof(priv), &n, (const unsigned char *) priv_b64, strlen(priv_b64)) != 0 || n != 32) {
        return false;
    }
    x25519(pub, priv, basepoint, 1);
    memset(priv, 0, sizeof(priv));
    size_t olen = 0;
    return mbedtls_base64_encode((unsigned char *) pub_b64, pub_size, &olen, pub, sizeof(pub)) == 0;
}

typedef struct {
    const char *priv;
} set_key_ctx_t;

static void update_key(app_settings_t *s, void *arg)
{
    strlcpy(s->vpn_private_key, ((set_key_ctx_t *) arg)->priv, sizeof(s->vpn_private_key));
}

bool vpn_link_generate_keys(char *pub_b64, size_t pub_size)
{
    unsigned char priv[32];
    esp_fill_random(priv, sizeof(priv));
    // "Clamping" della chiave privata Curve25519, come fa "wg genkey".
    priv[0] &= 248;
    priv[31] = (priv[31] & 127) | 64;
    char priv_b64[48];
    size_t olen = 0;
    bool ok = mbedtls_base64_encode((unsigned char *) priv_b64, sizeof(priv_b64), &olen, priv, sizeof(priv)) == 0;
    memset(priv, 0, sizeof(priv));
    if (ok) {
        ok = public_from_private(priv_b64, pub_b64, pub_size);
    }
    if (ok) {
        set_key_ctx_t c = { .priv = priv_b64 };
        ok = settings_update(update_key, &c) == ESP_OK;
    }
    memset(priv_b64, 0, sizeof(priv_b64));
    if (ok) {
        strlcpy(s_pub, pub_b64, sizeof(s_pub));
        ESP_LOGI(TAG, "Nuove chiavi VPN generate. Chiave pubblica della base: %s", pub_b64);
    }
    return ok;
}

static void stop_tunnel(const char *why)
{
    if (!s_started) {
        return;
    }
    ESP_LOGW(TAG, "Tunnel VPN chiuso: %s", why);
    // La libreria, chiudendo, rimette come rete predefinita quella del
    // momento del collegamento: se nel frattempo la base e' passata da WiFi
    // a SIM (o viceversa) sarebbe una rete spenta. Si usa quella attuale.
    s_ctx.netif_default = netif_default;
    esp_wireguard_disconnect(&s_ctx);
    s_started = false;
    s_up = false;
    s_started_net = -1;
}

static bool cfg_complete(const vpn_cfg_t *c)
{
    return c->priv[0] && c->addr[0] && c->peer[0] && c->endpoint[0];
}

void vpn_link_tick(void)
{
    static vpn_cfg_t now_cfg; // statico: niente struct sullo stack del chiamante
    settings_peek(peek_cfg, &now_cfg);
    int64_t now = esp_timer_get_time();

    if (!s_pub[0] && now_cfg.priv[0]) {
        public_from_private(now_cfg.priv, s_pub, sizeof(s_pub));
    }

    if (!now_cfg.enable || !cfg_complete(&now_cfg)) {
        stop_tunnel("VPN spenta o configurazione incompleta");
        if (now_cfg.enable) {
            set_error("configurazione incompleta (chiavi, server o indirizzo)");
        }
        return;
    }
    if (s_started && memcmp(&now_cfg, &s_cfg, sizeof(s_cfg)) != 0) {
        stop_tunnel("impostazioni cambiate");
    }

    // Rete in uso: WiFi/SIM da net_manager, oppure solo Ethernet.
    int net = (int) status_get_net();
    if (net == NET_STATUS_NONE && eth_link_is_connected()) {
        net = 100;
    }
    if (net == NET_STATUS_NONE) {
        stop_tunnel("rete assente");
        return;
    }
    if (s_started && net != s_started_net) {
        stop_tunnel("cambio di rete");
    }

    if (!s_started) {
        // WireGuard rifiuta gli handshake con l'orologio sbagliato.
        if (!time_sync_is_valid()) {
            set_error("in attesa dell'ora esatta (NTP)");
            return;
        }
        if (now < s_retry_after_us) {
            return;
        }
        s_cfg = now_cfg;
        s_wg_cfg = (wireguard_config_t) ESP_WIREGUARD_CONFIG_DEFAULT();
        s_wg_cfg.private_key = s_cfg.priv;
        s_wg_cfg.public_key = s_cfg.peer;
        s_wg_cfg.allowed_ip = s_cfg.addr;
        s_wg_cfg.allowed_ip_mask = "255.255.255.0";
        s_wg_cfg.endpoint = s_cfg.endpoint;
        s_wg_cfg.port = s_cfg.port;
        s_wg_cfg.persistent_keepalive = s_cfg.keepalive;
        if (!s_platform_ok) {
            if (esp_wireguard_init(&s_wg_cfg, &s_ctx) != ESP_OK) {
                set_error("inizializzazione fallita");
                s_retry_after_us = now + VPN_RETRY_US;
                return;
            }
            s_platform_ok = true;
        }
        s_ctx.config = &s_wg_cfg;
        s_ctx.netif_default = netif_default;
        esp_err_t err = esp_wireguard_connect(&s_ctx);
        if (err != ESP_OK) {
            // Tipicamente: nome del server non risolto (DNS) o chiave non valida.
            set_error("collegamento fallito (server non trovato o chiave non valida)");
            ESP_LOGW(TAG, "Collegamento VPN a %s:%u fallito: %s", s_cfg.endpoint, s_cfg.port, esp_err_to_name(err));
            if (s_ctx.netif) {
                s_started = true; // interfaccia creata: va chiusa per bene
                stop_tunnel("collegamento fallito");
            }
            s_retry_after_us = now + VPN_RETRY_US;
            return;
        }
        s_started = true;
        s_started_net = net;
        s_started_us = now;
        s_up = false;
        set_error("in attesa della risposta del server");
        ESP_LOGI(TAG, "Tunnel VPN avviato verso %s:%u, indirizzo della base %s", s_cfg.endpoint, s_cfg.port, s_cfg.addr);
        return;
    }

    bool up = esp_wireguardif_peer_is_up(&s_ctx) == ESP_OK;
    if (up && !s_up) {
        s_up_since_us = now;
        set_error("");
        ESP_LOGI(TAG, "VPN collegata: pannello raggiungibile su http://%s", s_cfg.addr);
    } else if (!up && s_up) {
        ESP_LOGW(TAG, "VPN: il server non risponde piu'");
        set_error("il server non risponde");
    }
    s_up = up;
    if (up) {
        s_last_up_us = now;
    } else {
        int64_t since = s_last_up_us > s_started_us ? s_last_up_us : s_started_us;
        if (now - since > VPN_DOWN_RESTART_US) {
            set_error("nessuna risposta dal server da 5 minuti: si riprova");
            stop_tunnel("nessuna risposta dal server da 5 minuti");
            s_retry_after_us = now + VPN_RETRY_US;
        }
    }
}

void vpn_link_get_status(vpn_status_t *out)
{
    vpn_cfg_t c;
    settings_peek(peek_cfg, &c);
    memset(out, 0, sizeof(*out));
    out->enabled = c.enable;
    out->configured = cfg_complete(&c);
    out->started = s_started;
    out->up = s_up;
    out->up_since_us = s_up ? s_up_since_us : 0;
    out->last_up_us = s_last_up_us;
    strlcpy(out->public_key, s_pub, sizeof(out->public_key));
    strlcpy(out->last_error, s_last_error, sizeof(out->last_error));
}
