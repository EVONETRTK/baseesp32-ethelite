#include "license.h"
#include "settings.h"
#include "status.h"
#include "time_sync.h"
#include "version.h"
#include "eth_link.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
#include "bootloader_random.h"
#include "esp_timer.h"
#include "esp_app_desc.h"
#include "esp_heap_caps.h"
#include "esp_http_client.h"
#include "esp_crt_bundle.h"
#include "nvs.h"
#include "cJSON.h"
#include "mbedtls/pk.h"
#include "mbedtls/ecp.h"
#include "mbedtls/sha256.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "license";

#define NVS_NS          "license"
#define DEFAULT_SERVER  "https://rtk.evo-net.it/api/device/v1/"
#define LIC_MAX         1536   // JSON della licenza
#define SIG_MAX         80     // firma ECDSA P-256 DER (max 72)
#define HTTP_BUF        4096
#define RETRY_S         (6 * 3600)
#define EXPIRING_S      (30 * 86400)
#define GRACE_S         (30 * 86400) // tolleranza senza contatto con il server (doc §1)
#define NOTE_DAYS       7            // da qui il pannello avvisa "licenza non rinnovata"
// Stack nella RAM interna, non nella PSRAM: il task scrive nella NVS (flash),
// e durante la scrittura la cache e' spenta e la PSRAM non si legge
// (CONFIG_SPIRAM_XIP_FROM_PSRAM non attivo): uno stack in PSRAM farebbe
// fermare il firmware. 10 KB per il dialogo HTTPS (TLS, ECDSA, cJSON); il
// task esiste solo durante un'attivazione o un rinnovo (una volta al giorno).
#define JOB_STACK       10240
#define BOOT_STACK      8192

// Chiavi pubbliche del server (docs/attivazione-licenze.md §4): fino a due,
// per poterla cambiare senza fermare i ricevitori. "test" e' la chiave del
// server finto per le prove sul PC: va TOLTA quando arriva quella vera.
static const struct {
    const char *kid;
    const char *pem;
} SERVER_KEYS[] = {
    { "test",
      "-----BEGIN PUBLIC KEY-----\n"
      "MFkwEwYHKoZIzj0CAQYIKoZIzj0DAQcDQgAEqQxGyyRavpSnULCp+gwbX6G//ILy\n"
      "0FXfNA1J3KngRXGpzuzJhZ8BBOJDgIikguMZwpdPYTgncVoiBPJjbKRjmg==\n"
      "-----END PUBLIC KEY-----\n" },
};

static const char *const FEATURE_CODES[LIC_COUNT] = { "base", "rover", "nmea_fast", "has", "ins" };

typedef struct {
    bool valid;
    bool revoked;
    char customer[24];
    int64_t issued;
    int64_t renew_after;
    int64_t expiry[LIC_COUNT]; // -1 = non compresa
} lic_t;

static SemaphoreHandle_t s_mutex;
static lic_t s_lic;                       // licenza valida salvata (aggiornata dai rinnovi)
static bool s_granted_boot[LIC_COUNT];    // fisse fino al riavvio
static char s_chip[13];
static char s_serial[33];
static char *s_dev_pub_b64;               // chiave pubblica del ricevitore (SPKI DER, base64)
static uint8_t *s_dev_key_der;            // privata (DER)
static size_t s_dev_key_len;
static int64_t s_last_contact;
static int64_t s_last_time;              // ultima ora valida salvata (vedi store_last_time)
static bool s_had_stored;                 // c'e' una licenza salvata (anche se non verificabile)
static int s_suspended_days;              // >0: extra sospesi in questo avvio (nessun contatto da N giorni)
static int64_t s_last_attempt_us;
static volatile bool s_busy;
static char s_msg[256];
static char s_server[96];

typedef struct {
    bool activate;
    license_activation_t act;
    SemaphoreHandle_t done;
} job_t;

// PSRAM se disponibile, altrimenti RAM interna (CONFIG_SPIRAM_IGNORE_NOTFOUND:
// una scheda senza PSRAM deve comunque poter leggere la sua licenza).
static void *psalloc(size_t n)
{
    void *p = heap_caps_malloc(n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    return p ? p : malloc(n);
}

static int rng(void *ctx, unsigned char *buf, size_t len)
{
    (void) ctx;
    esp_fill_random(buf, len);
    return 0;
}

static void set_msg(const char *m)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_msg, m, sizeof(s_msg));
    xSemaphoreGive(s_mutex);
    ESP_LOGI(TAG, "%s", m);
}

// Internet disponibile: WiFi, cellulare o anche solo il cavo Ethernet
// (status_get_net() non conta l'Ethernet, vedi net_util.h).
static bool net_up(void)
{
    return status_get_net() != NET_STATUS_NONE || eth_link_is_connected();
}

static bool time_ok(void)
{
    return time_sync_is_valid() && time(NULL) > 1700000000;
}

// --- NVS -----------------------------------------------------------------------
// ESP_OK, ESP_ERR_NVS_NOT_FOUND (chiave assente) o un altro errore di lettura.
static esp_err_t nvs_get_blob_alloc_err(nvs_handle_t h, const char *key, uint8_t **out, size_t *len)
{
    size_t n = 0;
    esp_err_t e = nvs_get_blob(h, key, NULL, &n);
    if (e != ESP_OK) {
        return e;
    }
    if (n == 0) {
        return ESP_ERR_NVS_NOT_FOUND;
    }
    uint8_t *b = psalloc(n + 1);
    if (!b) {
        return ESP_ERR_NO_MEM;
    }
    e = nvs_get_blob(h, key, b, &n);
    if (e != ESP_OK) {
        free(b);
        return e;
    }
    b[n] = 0;
    *out = b;
    *len = n;
    return ESP_OK;
}

static bool nvs_get_blob_alloc(nvs_handle_t h, const char *key, uint8_t **out, size_t *len)
{
    return nvs_get_blob_alloc_err(h, key, out, len) == ESP_OK;
}

// --- chiave del ricevitore ---------------------------------------------------------
static bool load_or_create_device_key(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    uint8_t *der = NULL;
    size_t len = 0;
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    esp_err_t rd = nvs_get_blob_alloc_err(h, "dkey", &der, &len);
    int pr = rd == ESP_OK ? mbedtls_pk_parse_key(&pk, der, len, NULL, 0, rng, NULL) : 0;
    if (rd == ESP_OK && pr == 0) {
        ok = true;
    } else if (rd == ESP_OK) {
        // Chiave letta ma non decodificabile (memoria per mbedTLS, blob
        // rovinato): MAI rigenerarla da qui, cancellerebbe per sempre quella
        // a cui e' legata la licenza. Per questo avvio niente licenza; la NVS
        // resta com'e' per l'assistenza.
        ESP_LOGE(TAG, "Chiave del ricevitore presente ma non leggibile (mbedTLS -0x%04x): non la rigenero", (unsigned) -pr);
        free(der);
        nvs_close(h);
        mbedtls_pk_free(&pk);
        return false;
    } else if (rd != ESP_ERR_NVS_NOT_FOUND) {
        // Chiave presente ma non leggibile ora (memoria, NVS): MAI rigenerarla,
        // la licenza e' legata a questa chiave. Per questo avvio niente licenza.
        ESP_LOGE(TAG, "Chiave del ricevitore non leggibile (%s): non la rigenero", esp_err_to_name(rd));
        nvs_close(h);
        mbedtls_pk_free(&pk);
        return false;
    } else {
        free(der);
        der = NULL;
        mbedtls_pk_free(&pk);
        mbedtls_pk_init(&pk);
        ESP_LOGI(TAG, "Genero la chiave del ricevitore (P-256)");
        uint8_t *buf = psalloc(256);
        // Prima che partano WiFi e modem il generatore casuale dell'ESP32 non ha
        // la sorgente di rumore della radio: la si accende apposta per la chiave,
        // che resta per sempre (vedi esp_random.h).
        bootloader_random_enable();
        bool gen = buf && mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
                   mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, NULL) == 0;
        bootloader_random_disable();
        if (gen) {
            int n = mbedtls_pk_write_key_der(&pk, buf, 256);
            if (n > 0) {
                der = psalloc((size_t) n);
                if (der) {
                    memcpy(der, buf + 256 - n, (size_t) n);
                    len = (size_t) n;
                    ok = nvs_set_blob(h, "dkey", der, len) == ESP_OK && nvs_commit(h) == ESP_OK;
                }
            }
        }
        free(buf);
    }
    nvs_close(h);
    if (ok) {
        uint8_t pub[128];
        int n = mbedtls_pk_write_pubkey_der(&pk, pub, sizeof(pub));
        size_t olen = 0;
        s_dev_pub_b64 = psalloc(200);
        if (n <= 0 || !s_dev_pub_b64 ||
            mbedtls_base64_encode((unsigned char *) s_dev_pub_b64, 199, &olen, pub + sizeof(pub) - n, (size_t) n) != 0) {
            ok = false;
        } else {
            s_dev_pub_b64[olen] = 0;
            s_dev_key_der = der;
            s_dev_key_len = len;
        }
    }
    if (!ok) {
        free(der);
        ESP_LOGE(TAG, "Chiave del ricevitore non disponibile");
    }
    mbedtls_pk_free(&pk);
    return ok;
}

// Firma ECDSA (DER, base64) dei byte dati con la chiave del ricevitore.
static bool device_sign_b64(const uint8_t *data, size_t len, char *out, size_t out_size)
{
    mbedtls_pk_context pk;
    mbedtls_pk_init(&pk);
    bool ok = false;
    uint8_t hash[32], sig[MBEDTLS_PK_SIGNATURE_MAX_SIZE];
    size_t sig_len = 0, olen = 0;
    if (s_dev_key_der && mbedtls_pk_parse_key(&pk, s_dev_key_der, s_dev_key_len, NULL, 0, rng, NULL) == 0 &&
        mbedtls_sha256(data, len, hash, 0) == 0 &&
        mbedtls_pk_sign(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sizeof(sig), &sig_len, rng, NULL) == 0 &&
        mbedtls_base64_encode((unsigned char *) out, out_size - 1, &olen, sig, sig_len) == 0) {
        out[olen] = 0;
        ok = true;
    }
    mbedtls_pk_free(&pk);
    return ok;
}

// --- verifica della licenza -----------------------------------------------------------
static bool verify_sig(const char *kid, const uint8_t *data, size_t len, const uint8_t *sig, size_t sig_len)
{
    for (size_t i = 0; i < sizeof(SERVER_KEYS) / sizeof(SERVER_KEYS[0]); i++) {
        if (strcmp(kid, SERVER_KEYS[i].kid) != 0) {
            continue;
        }
        mbedtls_pk_context pk;
        mbedtls_pk_init(&pk);
        uint8_t hash[32];
        bool ok = mbedtls_pk_parse_public_key(&pk, (const unsigned char *) SERVER_KEYS[i].pem,
                                              strlen(SERVER_KEYS[i].pem) + 1) == 0 &&
                  mbedtls_sha256(data, len, hash, 0) == 0 &&
                  mbedtls_pk_verify(&pk, MBEDTLS_MD_SHA256, hash, sizeof(hash), sig, sig_len) == 0;
        mbedtls_pk_free(&pk);
        return ok;
    }
    return false;
}

static bool parse_license(const uint8_t *data, size_t len, const uint8_t *sig, size_t sig_len, const char *kid,
                          lic_t *out, char *why, size_t why_size)
{
    memset(out, 0, sizeof(*out));
    if (!verify_sig(kid, data, len, sig, sig_len)) {
        snprintf(why, why_size, "firma della licenza non valida (chiave \"%s\")", kid);
        return false;
    }
    cJSON *j = cJSON_ParseWithLength((const char *) data, len);
    if (!j) {
        snprintf(why, why_size, "licenza illeggibile");
        return false;
    }
    bool ok = false;
    const cJSON *v = cJSON_GetObjectItem(j, "v");
    const cJSON *serial = cJSON_GetObjectItem(j, "serial");
    const cJSON *chip = cJSON_GetObjectItem(j, "chip");
    const cJSON *pub = cJSON_GetObjectItem(j, "device_pub");
    const cJSON *feat = cJSON_GetObjectItem(j, "features");
    if (!cJSON_IsNumber(v) || v->valueint != 1) {
        snprintf(why, why_size, "versione della licenza non supportata");
    } else if (!cJSON_IsString(serial) || strcmp(serial->valuestring, s_serial) != 0) {
        snprintf(why, why_size, "licenza di un'altra matricola");
    } else if (!cJSON_IsString(chip) || strcasecmp(chip->valuestring, s_chip) != 0) {
        snprintf(why, why_size, "licenza di un altro chip");
    } else if (!cJSON_IsString(pub) || !s_dev_pub_b64 || strcmp(pub->valuestring, s_dev_pub_b64) != 0) {
        snprintf(why, why_size, "licenza di un'altra chiave del ricevitore");
    } else if (!cJSON_IsObject(feat)) {
        snprintf(why, why_size, "licenza senza funzioni");
    } else {
        const cJSON *it = cJSON_GetObjectItem(j, "issued");
        out->issued = cJSON_IsNumber(it) ? (int64_t) it->valuedouble : 0;
        it = cJSON_GetObjectItem(j, "renew_after");
        out->renew_after = cJSON_IsNumber(it) ? (int64_t) it->valuedouble : 0;
        out->revoked = cJSON_IsTrue(cJSON_GetObjectItem(j, "revoked"));
        it = cJSON_GetObjectItem(j, "customer");
        if (cJSON_IsString(it)) {
            strlcpy(out->customer, it->valuestring, sizeof(out->customer));
        }
        for (int f = 0; f < LIC_COUNT; f++) {
            it = cJSON_GetObjectItem(feat, FEATURE_CODES[f]);
            out->expiry[f] = cJSON_IsNumber(it) ? (int64_t) it->valuedouble : -1;
        }
        out->valid = true;
        ok = true;
    }
    cJSON_Delete(j);
    return ok;
}

// Ultimo contatto, ultima ora valida e indirizzo del server: letti sempre,
// anche se la chiave del ricevitore o la licenza non si leggono. Senza,
// store_last_time ripartiva da 0 e poteva riscrivere un'ora piu' vecchia.
static void load_times(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        return; // namespace mai creato: nessun valore salvato
    }
    int64_t v = 0;
    if (nvs_get_i64(h, "contact", &v) == ESP_OK) {
        s_last_contact = v;
    }
    v = 0;
    if (nvs_get_i64(h, "lasttime", &v) == ESP_OK) {
        s_last_time = v;
    }
    size_t ul = sizeof(s_server);
    if (nvs_get_str(h, "url", s_server, &ul) != ESP_OK) {
        s_server[0] = 0;
    }
    nvs_close(h);
}

static bool load_stored(lic_t *out, char *why, size_t why_size)
{
    nvs_handle_t h;
    memset(out, 0, sizeof(*out));
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) {
        snprintf(why, why_size, "nessuna licenza");
        return false;
    }
    uint8_t *lic = NULL, *sig = NULL;
    size_t lic_len = 0, sig_len = 0;
    char kid[16] = "";
    size_t kl = sizeof(kid);
    bool ok = false;
    uint8_t *pack = NULL;
    size_t pack_len = 0;
    if (nvs_get_blob_alloc(h, "pack", &pack, &pack_len)) {
        // Formato in un solo blob (scrittura atomica): [kid_len][kid][sig_len lo/hi][sig][lic]
        s_had_stored = true;
        size_t kl8 = pack_len > 0 ? pack[0] : 0;
        if (kl8 > 0 && kl8 < sizeof(kid) && pack_len > 1 + kl8 + 2) {
            memcpy(kid, pack + 1, kl8);
            kid[kl8] = 0;
            size_t sl = pack[1 + kl8] | ((size_t) pack[2 + kl8] << 8);
            size_t off = 3 + kl8;
            if (sl > 0 && pack_len > off + sl) {
                ok = parse_license(pack + off + sl, pack_len - off - sl, pack + off, sl, kid, out, why, why_size);
            } else {
                snprintf(why, why_size, "licenza salvata rovinata");
            }
        } else {
            snprintf(why, why_size, "licenza salvata rovinata");
        }
    } else if (nvs_get_blob_alloc(h, "lic", &lic, &lic_len) && nvs_get_blob_alloc(h, "sig", &sig, &sig_len) &&
               nvs_get_str(h, "kid", kid, &kl) == ESP_OK) {
        // Formato delle 1.29.x (tre chiavi separate).
        s_had_stored = true;
        ok = parse_license(lic, lic_len, sig, sig_len, kid, out, why, why_size);
    } else {
        snprintf(why, why_size, "nessuna licenza");
    }
    free(pack);
    free(lic);
    free(sig);
    nvs_close(h);
    return ok;
}

// Licenza, firma e nome della chiave in un solo blob: la scrittura di una voce
// NVS e' atomica, quindi un calo di tensione lascia la licenza vecchia o la nuova,
// mai un miscuglio non piu' verificabile.
static bool store(const uint8_t *lic, size_t lic_len, const uint8_t *sig, size_t sig_len, const char *kid)
{
    size_t kl = strlen(kid);
    if (kl == 0 || kl > 15 || sig_len == 0 || sig_len > 0xffff) {
        return false;
    }
    size_t n = 3 + kl + sig_len + lic_len;
    uint8_t *pack = psalloc(n);
    if (!pack) {
        return false;
    }
    pack[0] = (uint8_t) kl;
    memcpy(pack + 1, kid, kl);
    pack[1 + kl] = (uint8_t) (sig_len & 0xff);
    pack[2 + kl] = (uint8_t) (sig_len >> 8);
    memcpy(pack + 3 + kl, sig, sig_len);
    memcpy(pack + 3 + kl + sig_len, lic, lic_len);
    nvs_handle_t h;
    bool ok = false;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        ok = nvs_set_blob(h, "pack", pack, n) == ESP_OK && nvs_commit(h) == ESP_OK;
        if (ok) {
            nvs_erase_key(h, "lic"); // formato vecchio, non piu' usato
            nvs_erase_key(h, "sig");
            nvs_erase_key(h, "kid");
            nvs_commit(h);
        }
        nvs_close(h);
    }
    free(pack);
    if (ok) {
        s_had_stored = true;
    }
    return ok;
}

// Ultima ora valida vista (salvata di tanto in tanto): all'avvio, senza NTP, le
// scadenze si confrontano con questa, che non puo' tornare indietro.
// Non scrive mai un valore piu' vecchio di quello gia' nella NVS (anche se
// in memoria non fosse stato letto). Solo reset_last_time la puo' abbassare.
static void store_last_time(int64_t t)
{
    if (t <= s_last_time + 3600) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        int64_t cur = 0;
        if (nvs_get_i64(h, "lasttime", &cur) == ESP_OK && cur >= t) {
            t = cur; // nella NVS c'e' gia' un'ora piu' recente
        } else {
            nvs_set_i64(h, "lasttime", t);
            nvs_commit(h);
        }
        nvs_close(h);
    }
    s_last_time = t;
}

// Ora portata indietro dall'ora del server (licenza firmata appena ricevuta):
// un orologio finito nel futuro (NTP, GNSS o modem sbagliati) aveva salvato
// un'ora che avrebbe fatto scadere gli extra per sempre.
static void reset_last_time(int64_t t)
{
    ESP_LOGW(TAG, "Ultima ora salvata (%lld) nel futuro rispetto al server: riportata a %lld",
             (long long) s_last_time, (long long) t);
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "lasttime", t);
        nvs_commit(h);
        nvs_close(h);
    }
    s_last_time = t;
}

static void store_contact(int64_t t)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_i64(h, "contact", t);
        nvs_commit(h);
        nvs_close(h);
    }
    s_last_contact = t;
}

// --- dialogo con il server -------------------------------------------------------------
static const char *server_base(void)
{
#if LICENSE_ENFORCE
    // Un indirizzo http:// salvato durante le prove non vale in esercizio.
    if (s_server[0] && strncmp(s_server, "https://", 8) != 0) {
        return DEFAULT_SERVER;
    }
#endif
    return s_server[0] ? s_server : DEFAULT_SERVER;
}

// POST {"req": base64(json), "sig": firma}. Ritorna lo status HTTP (<0 rete), corpo in resp.
static int post_signed(const char *path, const char *json, char *resp, size_t resp_size)
{
    size_t jl = strlen(json), b64_cap = (jl + 2) / 3 * 4 + 4, olen = 0;
    char *b64 = psalloc(b64_cap);
    char *body = psalloc(b64_cap + 200);
    char sig[160];
    int status = -1;
    resp[0] = 0;
    if (!b64 || !body || mbedtls_base64_encode((unsigned char *) b64, b64_cap, &olen, (const uint8_t *) json, jl) != 0 ||
        !device_sign_b64((const uint8_t *) json, jl, sig, sizeof(sig))) {
        free(b64);
        free(body);
        return -2;
    }
    b64[olen] = 0;
    int bl = snprintf(body, b64_cap + 200, "{\"req\":\"%s\",\"sig\":\"%s\"}", b64, sig);
    char url[160];
    snprintf(url, sizeof(url), "%s%s%s", server_base(), server_base()[strlen(server_base()) - 1] == '/' ? "" : "/", path);
    esp_http_client_config_t cfg = {
        .url = url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 15000,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .buffer_size = 2048,
        .buffer_size_tx = 1024,
    };
    esp_http_client_handle_t c = esp_http_client_init(&cfg);
    if (c) {
        esp_http_client_set_header(c, "Content-Type", "application/json");
        esp_http_client_set_header(c, "Accept-Encoding", "identity");
        if (esp_http_client_open(c, bl) == ESP_OK && esp_http_client_write(c, body, bl) == bl &&
            esp_http_client_fetch_headers(c) >= 0) {
            status = esp_http_client_get_status_code(c);
            int got = 0, n;
            while (got < (int) resp_size - 1 && (n = esp_http_client_read(c, resp + got, (int) resp_size - 1 - got)) > 0) {
                got += n;
            }
            resp[got] = 0;
        }
        esp_http_client_cleanup(c);
    }
    free(b64);
    free(body);
    return status;
}

static void server_error_msg(int status, const char *resp, const char *what)
{
    char m[256];
    cJSON *j = status > 0 ? cJSON_Parse(resp) : NULL;
    const cJSON *msg = j ? cJSON_GetObjectItem(j, "message") : NULL;
    if (cJSON_IsString(msg)) {
        snprintf(m, sizeof(m), "%s: %s", what, msg->valuestring);
    } else if (status < 0) {
        snprintf(m, sizeof(m), "%s: server non raggiungibile (rete o firewall)", what);
    } else {
        snprintf(m, sizeof(m), "%s: risposta %d del server", what, status);
    }
    cJSON_Delete(j);
    set_msg(m);
}

// Contatto riuscito con il server: con una licenza nuova vale la sua ora di
// emissione (ora del server, firmata); altrimenti l'ora del ricevitore se
// valida. Senza ora valida non si aggiorna (prima si salvava 0 = "mai").
static void note_contact(bool is_new, int64_t issued)
{
    if (is_new && issued > 0) {
        store_contact(issued);
    } else if (time_ok()) {
        store_contact((int64_t) time(NULL));
    }
}

// Risposta {license, sig, kid}: verifica e salva. Ritorna true se e' diventata
// la licenza corrente; *is_new = emessa dopo quella che c'era (o prima licenza),
// *issued = la sua ora di emissione.
static bool accept_license(const char *resp, char *why, size_t why_size, bool *is_new, int64_t *issued)
{
    *is_new = false;
    *issued = 0;
    cJSON *j = cJSON_Parse(resp);
    const cJSON *l = j ? cJSON_GetObjectItem(j, "license") : NULL;
    const cJSON *s = j ? cJSON_GetObjectItem(j, "sig") : NULL;
    const cJSON *k = j ? cJSON_GetObjectItem(j, "kid") : NULL;
    bool ok = false;
    uint8_t *lic = psalloc(LIC_MAX);
    uint8_t sig[SIG_MAX];
    size_t ll = 0, sl = 0;
    lic_t parsed;
    if (!cJSON_IsString(l) || !cJSON_IsString(s) || !cJSON_IsString(k) || !lic ||
        mbedtls_base64_decode(lic, LIC_MAX, &ll, (const uint8_t *) l->valuestring, strlen(l->valuestring)) != 0 ||
        mbedtls_base64_decode(sig, sizeof(sig), &sl, (const uint8_t *) s->valuestring, strlen(s->valuestring)) != 0) {
        snprintf(why, why_size, "risposta del server non valida");
    } else if (parse_license(lic, ll, sig, sl, k->valuestring, &parsed, why, why_size)) {
        if (s_lic.valid && parsed.issued < s_lic.issued) {
            snprintf(why, why_size, "licenza piu' vecchia di quella attuale");
        } else if (!store(lic, ll, sig, sl, k->valuestring)) {
            snprintf(why, why_size, "salvataggio non riuscito");
        } else {
            *is_new = !s_lic.valid || parsed.issued > s_lic.issued;
            *issued = parsed.issued;
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_lic = parsed;
            xSemaphoreGive(s_mutex);
            ok = true;
            // L'ora del server fa fede: ultima ora salvata e ultimo contatto
            // oltre un giorno dopo l'emissione di una licenza appena firmata
            // vengono da un orologio sbagliato nel futuro.
            if (*is_new && parsed.issued > 0) {
                if (s_last_time > parsed.issued + 86400) {
                    reset_last_time(parsed.issued);
                }
                if (s_last_contact > parsed.issued + 86400) {
                    store_contact(parsed.issued);
                }
            }
        }
    }
    free(lic);
    cJSON_Delete(j);
    return ok;
}

static void add_common(cJSON *r)
{
    // Impronta completa dell'ELF (esp_app_get_elf_sha256 ne da' solo 9 cifre,
    // CONFIG_APP_RETRIEVE_LEN_ELF_SHA): il server la confronta con le release.
    char sha[65];
    const esp_app_desc_t *desc = esp_app_get_description();
    for (int i = 0; i < 32; i++) {
        snprintf(sha + 2 * i, 3, "%02x", desc->app_elf_sha256[i]);
    }
    uint8_t nonce[16];
    char nb[32];
    size_t olen = 0;
    esp_fill_random(nonce, sizeof(nonce));
    mbedtls_base64_encode((unsigned char *) nb, sizeof(nb), &olen, nonce, sizeof(nonce));
    nb[olen] = 0;
    cJSON_AddStringToObject(r, "serial", s_serial);
    cJSON_AddStringToObject(r, "chip", s_chip);
    cJSON_AddStringToObject(r, "fw", FIRMWARE_VERSION);
    cJSON_AddStringToObject(r, "fw_sha256", sha);
    cJSON_AddStringToObject(r, "nonce", nb);
    cJSON_AddNumberToObject(r, "ts", time_ok() ? (double) time(NULL) : 0);
}

static void do_activate(const license_activation_t *a)
{
    app_settings_t *cfg = psalloc(sizeof(app_settings_t));
    char *resp = psalloc(HTTP_BUF);
    cJSON *r = cJSON_CreateObject();
    if (!cfg || !resp || !r) {
        set_msg("Attivazione non riuscita: memoria insufficiente");
        goto out;
    }
    settings_get_into(cfg);
    add_common(r);
    cJSON_AddStringToObject(r, "device_pub", s_dev_pub_b64);
    cJSON_AddStringToObject(r, "code", a->code);
    cJSON_AddStringToObject(r, "mode", cfg->device_mode == DEVICE_MODE_ROVER ? "rover" : "base");
    cJSON *acc = cJSON_AddObjectToObject(r, "accept");
    cJSON_AddStringToObject(acc, "terms_version", LICENSE_TERMS_VERSION);
    cJSON_AddStringToObject(acc, "clauses_version", LICENSE_TERMS_VERSION);
    cJSON_AddBoolToObject(acc, "terms", a->terms);
    cJSON_AddBoolToObject(acc, "clauses_1341", a->clauses_1341);
    cJSON_AddStringToObject(acc, "who", a->who);
    cJSON_AddNumberToObject(acc, "at", time_ok() ? (double) time(NULL) : 0);
    char *json = cJSON_PrintUnformatted(r);
    if (!json) {
        set_msg("Attivazione non riuscita: memoria insufficiente");
        goto out;
    }
    int st = post_signed("activate", json, resp, HTTP_BUF);
    free(json);
    if (st == 200) {
        char why[96];
        bool is_new;
        int64_t issued;
        if (accept_license(resp, why, sizeof(why), &is_new, &issued)) {
            note_contact(is_new, issued);
            set_msg("Ricevitore attivato. Riavvialo per usare le funzioni della licenza.");
        } else {
            char m[256];
            snprintf(m, sizeof(m), "Attivazione non riuscita: %s", why);
            set_msg(m);
        }
    } else {
        server_error_msg(st, resp, "Attivazione non riuscita");
    }
out:
    cJSON_Delete(r);
    free(resp);
    free(cfg);
}

static void do_renew(void)
{
    app_settings_t *cfg = psalloc(sizeof(app_settings_t));
    char *resp = psalloc(HTTP_BUF);
    cJSON *r = cJSON_CreateObject();
    if (!cfg || !resp || !r) {
        set_msg("Rinnovo non riuscito: memoria insufficiente");
        goto out;
    }
    settings_get_into(cfg);
    add_common(r);
    cJSON_AddNumberToObject(r, "issued", s_lic.valid ? (double) s_lic.issued : 0);
    // Funzioni in uso, per il confronto con la licenza sul server (§8.4).
    cJSON *use = cJSON_AddArrayToObject(r, "in_use");
    cJSON_AddItemToArray(use, cJSON_CreateString(cfg->device_mode == DEVICE_MODE_ROVER ? "rover" : "base"));
    if (cfg->nmea_rate_hz > 1) {
        cJSON_AddItemToArray(use, cJSON_CreateString("nmea_fast"));
    }
    if (cfg->rover_has_fallback && cfg->device_mode == DEVICE_MODE_ROVER) {
        cJSON_AddItemToArray(use, cJSON_CreateString("has"));
    }
    if (cfg->comnav_ins_enable || cfg->comnav_heading_enable) { // IMU o prua a doppia antenna
        cJSON_AddItemToArray(use, cJSON_CreateString("ins"));
    }
    if (cfg->device_mode == DEVICE_MODE_ROVER &&
        (cfg->ntrip_caster_server_enable || cfg->vpn_enable || cfg->remote_interval_min || cfg->alert_enable)) {
        cJSON_AddItemToArray(use, cJSON_CreateString("base")); // servizi del Pacchetto Base usati su un rover
    }
    char *json = cJSON_PrintUnformatted(r);
    if (!json) {
        set_msg("Rinnovo non riuscito: memoria insufficiente");
        goto out;
    }
    int st = post_signed("renew", json, resp, HTTP_BUF);
    free(json);
    if (st == 304) {
        // Risposta senza firma (nessuna licenza nel corpo): conta come
        // contatto con l'ora del ricevitore, se valida.
        note_contact(false, 0);
        set_msg("Licenza controllata: nessuna novita'");
    } else if (st == 200) {
        char why[96];
        bool is_new;
        int64_t issued;
        if (accept_license(resp, why, sizeof(why), &is_new, &issued)) {
            note_contact(is_new, issued);
            set_msg(s_lic.revoked ? "Licenza aggiornata: ricevitore REVOCATO dal server (vale dal riavvio)"
                                  : "Licenza aggiornata (le novita' valgono dal riavvio)");
        } else {
            char m[256];
            snprintf(m, sizeof(m), "Rinnovo non riuscito: %s", why);
            set_msg(m);
        }
    } else {
        server_error_msg(st, resp, "Rinnovo non riuscito");
    }
out:
    cJSON_Delete(r);
    free(resp);
    free(cfg);
}

static void job_task(void *arg)
{
    job_t *job = arg;
    if (job->activate) {
        do_activate(&job->act);
    } else {
        do_renew();
    }
    ESP_LOGI(TAG, "Stack libero minimo %u byte", (unsigned) uxTaskGetStackHighWaterMark(NULL));
    free(job);
    s_busy = false;
    vTaskDelete(NULL);
}

// "Operazione in corso": controllo e impostazione insieme, sotto il mutex
// (pannello e controllo automatico possono chiederlo nello stesso momento).
static bool take_busy(void)
{
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    bool ok = !s_busy;
    s_busy = true;
    xSemaphoreGive(s_mutex);
    return ok;
}

static bool start_job(job_t *job, char *err, size_t err_size)
{
    if (!take_busy()) {
        snprintf(err, err_size, "operazione gia' in corso");
        free(job);
        return false;
    }
    s_last_attempt_us = esp_timer_get_time();
    if (xTaskCreate(job_task, "license", JOB_STACK, job, 3, NULL) != pdPASS) {
        s_busy = false;
        free(job);
        snprintf(err, err_size, "memoria insufficiente");
        return false;
    }
    return true;
}

// --- avvio ---------------------------------------------------------------------
static void boot_task(void *arg)
{
    SemaphoreHandle_t done = arg;
    char why[96] = "";
    load_times();
    if (load_or_create_device_key()) {
        lic_t l;
        if (load_stored(&l, why, sizeof(why))) {
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_lic = l;
            xSemaphoreGive(s_mutex);
        }
    } else {
        snprintf(why, sizeof(why), "chiave del ricevitore non disponibile");
    }
    // Funzioni di questo avvio. All'avvio l'ora esatta (NTP) di solito non c'e'
    // ancora: si usa l'istante piu' recente certamente gia' passato (emissione
    // della licenza firmata, ultimo contatto, ultima ora salvata), che non puo'
    // tornare indietro. Cosi' una funzione scaduta non resta attiva per sempre.
    int64_t now = time_ok() ? (int64_t) time(NULL) : 0;
    if (s_lic.issued > now) now = s_lic.issued;
    if (s_last_contact > now) now = s_last_contact;
    if (s_last_time > now) now = s_last_time;
    for (int f = 0; f < LIC_COUNT; f++) {
        int64_t e = s_lic.expiry[f];
        s_granted_boot[f] = s_lic.valid && !s_lic.revoked && e >= 0 && (e == 0 || e > now);
    }
    // Gli extra del rover valgono solo con il Pacchetto Rover.
    if (!s_granted_boot[LIC_ROVER]) {
        s_granted_boot[LIC_NMEA_FAST] = s_granted_boot[LIC_HAS] = s_granted_boot[LIC_INS] = false;
    }
    // Tolleranza di 30 giorni (doc §1): passato renew_after, se il server non
    // risponde da oltre 30 giorni gli extra non si concedono in questo avvio;
    // i pacchetti restano. Riferimento: il piu' recente fra ultimo contatto
    // riuscito ed emissione della licenza (contatto 0 = mai, dopo
    // l'attivazione vale l'emissione). "now" qui non e' mai piu' avanti
    // dell'ora vera (vedi sopra): senza ora esatta si sospende al massimo piu' tardi.
    {
        int64_t ref = s_last_contact > s_lic.issued ? s_last_contact : s_lic.issued;
        bool extras = s_granted_boot[LIC_NMEA_FAST] || s_granted_boot[LIC_HAS] || s_granted_boot[LIC_INS];
        if (s_lic.valid && extras && ref > 0 && s_lic.renew_after > 0 && now >= s_lic.renew_after &&
            now - ref > GRACE_S) {
            s_suspended_days = (int) ((now - ref) / 86400);
            s_granted_boot[LIC_NMEA_FAST] = s_granted_boot[LIC_HAS] = s_granted_boot[LIC_INS] = false;
            char m[160];
            snprintf(m, sizeof(m), "Licenza non rinnovata da %d giorni: funzioni extra sospese finche' il ricevitore "
                     "non si collega al server", s_suspended_days);
            strlcpy(s_msg, m, sizeof(s_msg)); // nessun altro task usa ancora s_msg
            ESP_LOGW(TAG, "%s", m);
        }
    }
    if (!s_lic.valid) {
        ESP_LOGW(TAG, "Ricevitore NON attivato (%s)%s", why, LICENSE_ENFORCE ? "" : " - modalita' di prova: nessun blocco");
    } else {
        ESP_LOGI(TAG, "Licenza valida: cliente %s, %s, funzioni base=%d rover=%d nmea_fast=%d has=%d ins=%d%s",
                 s_lic.customer, s_lic.revoked ? "REVOCATA" : "attiva", s_granted_boot[0], s_granted_boot[1],
                 s_granted_boot[2], s_granted_boot[3], s_granted_boot[4], LICENSE_ENFORCE ? "" : " (modalita' di prova)");
    }
    ESP_LOGI(TAG, "Verifica all'avvio: stack libero minimo %u byte", (unsigned) uxTaskGetStackHighWaterMark(NULL));
    xSemaphoreGive(done);
    vTaskDelete(NULL);
}

void license_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    uint8_t mac[6];
    esp_efuse_mac_get_default(mac);
    snprintf(s_chip, sizeof(s_chip), "%02x%02x%02x%02x%02x%02x", mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
    app_settings_t *cfg = psalloc(sizeof(app_settings_t));
    if (cfg) {
        settings_get_into(cfg);
        strlcpy(s_serial, cfg->device_serial, sizeof(s_serial));
        free(cfg);
    }
    for (int f = 0; f < LIC_COUNT; f++) {
        s_lic.expiry[f] = -1;
    }
    // La verifica (ECDSA) usa qualche KB di stack: in un task a parte, aspettato qui.
    SemaphoreHandle_t done = xSemaphoreCreateBinary();
    // Si aspetta senza limite: le funzioni concesse vanno decise prima di
    // avviare i servizi, e il semaforo non si libera mentre il task lo usa.
    if (!done) {
        ESP_LOGE(TAG, "Memoria insufficiente per la verifica della licenza");
    } else if (xTaskCreate(boot_task, "license_boot", BOOT_STACK, done, 5, NULL) == pdPASS) {
        xSemaphoreTake(done, portMAX_DELAY);
        vSemaphoreDelete(done);
    } else {
        ESP_LOGE(TAG, "Impossibile avviare la verifica della licenza: nessuna funzione concessa");
        vSemaphoreDelete(done);
    }
}

// --- interrogazioni --------------------------------------------------------------------
// La funzione conta? I pacchetti si; gli extra solo se la licenza comprende
// il Pacchetto Rover (non scaduto, se l'ora e' nota), come in boot_task.
static bool extra_counts(const lic_t *l, int f)
{
    if (f < LIC_NMEA_FAST) {
        return true;
    }
    int64_t r = l->expiry[LIC_ROVER];
    return r == 0 || (r > 0 && (!time_ok() || r > (int64_t) time(NULL)));
}

bool license_granted(license_feature_t f)
{
    return f < LIC_COUNT && s_granted_boot[f];
}

bool license_has(license_feature_t f)
{
#if LICENSE_ENFORCE
    return license_granted(f);
#else
    (void) f;
    return true;
#endif
}

const char *license_feature_code(license_feature_t f)
{
    return f < LIC_COUNT ? FEATURE_CODES[f] : "";
}

void license_get_status(license_status_t *out)
{
    memset(out, 0, sizeof(*out));
    if (!s_mutex) {
        return;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lic_t l = s_lic;
    strlcpy(out->last_msg, s_msg, sizeof(out->last_msg));
    xSemaphoreGive(s_mutex);
    out->enforce = LICENSE_ENFORCE;
    strlcpy(out->chip, s_chip, sizeof(out->chip));
    strlcpy(out->customer, l.customer, sizeof(out->customer));
    strlcpy(out->server, server_base(), sizeof(out->server));
    out->issued = l.issued;
    out->renew_after = l.renew_after;
    out->last_contact = s_last_contact;
    out->busy = s_busy;
    out->stored = license_is_stored();
    out->extras_suspended = s_suspended_days > 0;
    out->no_contact_days = -1;
    {
        int64_t ref = s_last_contact > l.issued ? s_last_contact : l.issued;
        if (l.valid && ref > 0 && time_ok() && (int64_t) time(NULL) >= ref) {
            out->no_contact_days = (int) (((int64_t) time(NULL) - ref) / 86400);
        }
    }
    for (int f = 0; f < LIC_COUNT; f++) {
        out->expiry[f] = l.valid ? l.expiry[f] : -1;
        out->granted_boot[f] = s_granted_boot[f];
    }
    if (!l.valid) {
        out->state = LIC_STATE_UNACTIVATED;
        return;
    }
    if (l.revoked) {
        out->state = LIC_STATE_REVOKED;
        return;
    }
    out->state = LIC_STATE_ACTIVE;
    if (time_ok()) {
        int64_t now = (int64_t) time(NULL);
        for (int f = 0; f < LIC_COUNT; f++) { // di norma solo gli extra hanno una scadenza
            if (!extra_counts(&l, f)) {
                continue; // extra senza Pacchetto Rover: non vale comunque, la scadenza non conta
            }
            int64_t e = l.expiry[f];
            if (e > 0 && e <= now) {
                out->state = LIC_STATE_EXTRA_EXPIRED;
                break;
            }
            if (e > 0 && e - now < EXPIRING_S) {
                out->state = LIC_STATE_EXPIRING;
            }
        }
    }
    // Tolleranza senza contatto con il server (vedi boot_task).
    bool has_extras = false;
    for (int f = LIC_NMEA_FAST; f < LIC_COUNT; f++) {
        has_extras |= l.expiry[f] >= 0 && extra_counts(&l, f);
    }
    if (out->extras_suspended) {
        if (out->no_contact_days >= 0 && out->no_contact_days < s_suspended_days) {
            snprintf(out->note, sizeof(out->note),
                     "Licenza rinnovata: le funzioni extra sospese tornano dal prossimo riavvio");
        } else {
            snprintf(out->note, sizeof(out->note),
                     "Licenza non rinnovata da %d giorni: funzioni extra sospese finche' il ricevitore non si collega al server",
                     out->no_contact_days > s_suspended_days ? out->no_contact_days : s_suspended_days);
        }
    } else if (has_extras && out->no_contact_days >= NOTE_DAYS && out->renew_after > 0 &&
               time_ok() && (int64_t) time(NULL) >= out->renew_after) {
        snprintf(out->note, sizeof(out->note),
                 "Licenza non rinnovata da %d giorni: senza collegamento al server le funzioni extra si sospendono dopo 30 giorni",
                 out->no_contact_days);
    }
    if (!out->last_msg[0] && out->note[0]) {
        strlcpy(out->last_msg, out->note, sizeof(out->last_msg)); // il pannello di oggi mostra last_msg
    }
}

bool license_is_stored(void)
{
    return s_had_stored || s_lic.valid;
}

bool license_next_extra_expiry(int64_t *expiry)
{
    if (!s_mutex) {
        return false;
    }
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    lic_t l = s_lic;
    xSemaphoreGive(s_mutex);
    if (!l.valid || l.revoked) {
        return false;
    }
    int64_t best = 0;
    for (int f = LIC_NMEA_FAST; f < LIC_COUNT; f++) {
        int64_t e = l.expiry[f];
        if (e > 0 && extra_counts(&l, f) && (best == 0 || e < best)) {
            best = e;
        }
    }
    *expiry = best;
    return best > 0;
}

// --- richieste ---------------------------------------------------------------------------
bool license_request_activate(const license_activation_t *a, char *err, size_t err_size)
{
    if (!a->terms || !a->clauses_1341) {
        snprintf(err, err_size, "servono entrambe le spunte di accettazione");
        return false;
    }
    if (strlen(a->code) < 4) {
        snprintf(err, err_size, "codice di attivazione mancante");
        return false;
    }
    if (strlen(a->who) < 3) {
        snprintf(err, err_size, "scrivi nome e cognome di chi accetta");
        return false;
    }
    if (!s_dev_pub_b64) {
        snprintf(err, err_size, "chiave del ricevitore non disponibile");
        return false;
    }
    if (!net_up()) {
        snprintf(err, err_size, "serve internet: collega prima il ricevitore a una rete");
        return false;
    }
    job_t *job = calloc(1, sizeof(job_t));
    if (!job) {
        snprintf(err, err_size, "memoria insufficiente");
        return false;
    }
    job->activate = true;
    job->act = *a;
    set_msg("Attivazione in corso...");
    return start_job(job, err, err_size);
}

bool license_request_renew(char *err, size_t err_size)
{
    // Anche con una licenza salvata ma non piu' verificabile (chiave del server
    // cambiata, codici vecchi): la richiesta e' firmata dalla chiave del
    // ricevitore e il server rimanda la licenza giusta.
    if (!s_lic.valid && !s_had_stored) {
        snprintf(err, err_size, "ricevitore non ancora attivato");
        return false;
    }
    if (!s_dev_pub_b64) {
        snprintf(err, err_size, "chiave del ricevitore non disponibile");
        return false;
    }
    if (!net_up()) {
        snprintf(err, err_size, "nessuna connessione a internet");
        return false;
    }
    job_t *job = calloc(1, sizeof(job_t));
    if (!job) {
        snprintf(err, err_size, "memoria insufficiente");
        return false;
    }
    set_msg("Controllo della licenza in corso...");
    return start_job(job, err, err_size);
}

void license_tick(void)
{
    if (time_ok()) {
        store_last_time((int64_t) time(NULL));
    }
    if (s_busy || (!s_lic.valid && !s_had_stored) || !time_ok() || !net_up()) {
        return;
    }
    int64_t now = (int64_t) time(NULL);
    bool due = !s_lic.valid || now >= s_lic.renew_after || now - s_last_contact > 86400;
    if (!due) {
        return;
    }
    if (s_last_attempt_us != 0 && esp_timer_get_time() - s_last_attempt_us < (int64_t) RETRY_S * 1000000) {
        return;
    }
    char err[64];
    license_request_renew(err, sizeof(err));
}

bool license_set_server(const char *url, char *err, size_t err_size)
{
#if LICENSE_ENFORCE
    // In esercizio solo HTTPS: con http:// un intermediario potrebbe
    // rispondere al posto del server (il 304 non e' firmato).
    if (url[0] && strncmp(url, "https://", 8) != 0) {
        snprintf(err, err_size, "indirizzo non valido: serve https://");
        return false;
    }
#else
    // Modalita' di prova: anche http:// (server finto sul PC).
    if (url[0] && strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        snprintf(err, err_size, "indirizzo non valido");
        return false;
    }
#endif
    if (strlen(url) >= sizeof(s_server)) {
        snprintf(err, err_size, "indirizzo troppo lungo");
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        snprintf(err, err_size, "NVS non disponibile");
        return false;
    }
    if (url[0]) {
        nvs_set_str(h, "url", url);
    } else {
        nvs_erase_key(h, "url");
    }
    nvs_commit(h);
    nvs_close(h);
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    strlcpy(s_server, url, sizeof(s_server));
    xSemaphoreGive(s_mutex);
    return true;
}

bool license_forget(char *err, size_t err_size)
{
    if (!take_busy()) {
        snprintf(err, err_size, "operazione in corso");
        return false;
    }
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        s_busy = false;
        snprintf(err, err_size, "NVS non disponibile");
        return false;
    }
    nvs_erase_key(h, "pack");
    nvs_erase_key(h, "lic");
    nvs_erase_key(h, "sig");
    nvs_erase_key(h, "kid");
    nvs_erase_key(h, "contact");
    nvs_erase_key(h, "lasttime"); // anche un'ora sbagliata nel futuro rimasta salvata
    nvs_commit(h);
    nvs_close(h);
    s_last_time = 0;
    xSemaphoreTake(s_mutex, portMAX_DELAY);
    memset(&s_lic, 0, sizeof(s_lic));
    for (int f = 0; f < LIC_COUNT; f++) {
        s_lic.expiry[f] = -1;
    }
    s_busy = false;
    xSemaphoreGive(s_mutex);
    s_last_contact = 0;
    s_had_stored = false;
    set_msg("Licenza dimenticata: il ricevitore e' da attivare (le funzioni di questo avvio restano fino al riavvio)");
    return true;
}
