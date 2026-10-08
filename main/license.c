#include "license.h"
#include "settings.h"
#include "status.h"
#include "time_sync.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <time.h>

#include "esp_log.h"
#include "esp_mac.h"
#include "esp_random.h"
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
#define TASK_STACK      8192

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

static const char *const FEATURE_CODES[LIC_COUNT] = { "rtk", "nmea_fast", "has", "ins", "base_pro" };

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
static int64_t s_last_attempt_us;
static volatile bool s_busy;
static char s_msg[128];
static char s_server[96];

typedef struct {
    bool activate;
    license_activation_t act;
    SemaphoreHandle_t done;
} job_t;

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

static bool time_ok(void)
{
    return time_sync_is_valid() && time(NULL) > 1700000000;
}

// --- NVS -----------------------------------------------------------------------
static bool nvs_get_blob_alloc(nvs_handle_t h, const char *key, uint8_t **out, size_t *len)
{
    size_t n = 0;
    if (nvs_get_blob(h, key, NULL, &n) != ESP_OK || n == 0) {
        return false;
    }
    uint8_t *b = heap_caps_malloc(n + 1, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!b) {
        return false;
    }
    if (nvs_get_blob(h, key, b, &n) != ESP_OK) {
        free(b);
        return false;
    }
    b[n] = 0;
    *out = b;
    *len = n;
    return true;
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
    if (nvs_get_blob_alloc(h, "dkey", &der, &len) &&
        mbedtls_pk_parse_key(&pk, der, len, NULL, 0, rng, NULL) == 0) {
        ok = true;
    } else {
        free(der);
        der = NULL;
        mbedtls_pk_free(&pk);
        mbedtls_pk_init(&pk);
        ESP_LOGI(TAG, "Genero la chiave del ricevitore (P-256)");
        uint8_t *buf = heap_caps_malloc(256, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (buf && mbedtls_pk_setup(&pk, mbedtls_pk_info_from_type(MBEDTLS_PK_ECKEY)) == 0 &&
            mbedtls_ecp_gen_key(MBEDTLS_ECP_DP_SECP256R1, mbedtls_pk_ec(pk), rng, NULL) == 0) {
            int n = mbedtls_pk_write_key_der(&pk, buf, 256);
            if (n > 0) {
                der = heap_caps_malloc((size_t) n, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        s_dev_pub_b64 = heap_caps_malloc(200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    int64_t contact = 0;
    nvs_get_i64(h, "contact", &contact);
    s_last_contact = contact;
    size_t ul = sizeof(s_server);
    if (nvs_get_str(h, "url", s_server, &ul) != ESP_OK) {
        s_server[0] = 0;
    }
    bool ok = false;
    if (nvs_get_blob_alloc(h, "lic", &lic, &lic_len) && nvs_get_blob_alloc(h, "sig", &sig, &sig_len) &&
        nvs_get_str(h, "kid", kid, &kl) == ESP_OK) {
        ok = parse_license(lic, lic_len, sig, sig_len, kid, out, why, why_size);
    } else {
        snprintf(why, why_size, "nessuna licenza");
    }
    free(lic);
    free(sig);
    nvs_close(h);
    return ok;
}

static bool store(const uint8_t *lic, size_t lic_len, const uint8_t *sig, size_t sig_len, const char *kid)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return false;
    }
    bool ok = nvs_set_blob(h, "lic", lic, lic_len) == ESP_OK && nvs_set_blob(h, "sig", sig, sig_len) == ESP_OK &&
              nvs_set_str(h, "kid", kid) == ESP_OK && nvs_commit(h) == ESP_OK;
    nvs_close(h);
    return ok;
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
    return s_server[0] ? s_server : DEFAULT_SERVER;
}

// POST {"req": base64(json), "sig": firma}. Ritorna lo status HTTP (<0 rete), corpo in resp.
static int post_signed(const char *path, const char *json, char *resp, size_t resp_size)
{
    size_t jl = strlen(json), b64_cap = (jl + 2) / 3 * 4 + 4, olen = 0;
    char *b64 = heap_caps_malloc(b64_cap, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *body = heap_caps_malloc(b64_cap + 200, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    char m[128];
    cJSON *j = status > 0 ? cJSON_Parse(resp) : NULL;
    const cJSON *msg = j ? cJSON_GetObjectItem(j, "message") : NULL;
    if (cJSON_IsString(msg)) {
        snprintf(m, sizeof(m), "%s non riuscito: %s", what, msg->valuestring);
    } else if (status < 0) {
        snprintf(m, sizeof(m), "%s non riuscito: server non raggiungibile (rete o firewall)", what);
    } else {
        snprintf(m, sizeof(m), "%s non riuscito: risposta %d del server", what, status);
    }
    cJSON_Delete(j);
    set_msg(m);
}

// Risposta {license, sig, kid}: verifica e salva. Ritorna true se e' diventata la licenza corrente.
static bool accept_license(const char *resp, char *why, size_t why_size)
{
    cJSON *j = cJSON_Parse(resp);
    const cJSON *l = j ? cJSON_GetObjectItem(j, "license") : NULL;
    const cJSON *s = j ? cJSON_GetObjectItem(j, "sig") : NULL;
    const cJSON *k = j ? cJSON_GetObjectItem(j, "kid") : NULL;
    bool ok = false;
    uint8_t *lic = heap_caps_malloc(LIC_MAX, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
            xSemaphoreTake(s_mutex, portMAX_DELAY);
            s_lic = parsed;
            xSemaphoreGive(s_mutex);
            ok = true;
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
    app_settings_t *cfg = heap_caps_malloc(sizeof(app_settings_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *resp = heap_caps_malloc(HTTP_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
        if (accept_license(resp, why, sizeof(why))) {
            store_contact(time_ok() ? (int64_t) time(NULL) : 0);
            set_msg("Ricevitore attivato. Riavvialo per usare le funzioni della licenza.");
        } else {
            char m[128];
            snprintf(m, sizeof(m), "Attivazione non riuscita: %s", why);
            set_msg(m);
        }
    } else {
        server_error_msg(st, resp, "Attivazione");
    }
out:
    cJSON_Delete(r);
    free(resp);
    free(cfg);
}

static void do_renew(void)
{
    app_settings_t *cfg = heap_caps_malloc(sizeof(app_settings_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    char *resp = heap_caps_malloc(HTTP_BUF, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    cJSON *r = cJSON_CreateObject();
    if (!cfg || !resp || !r) {
        set_msg("Rinnovo non riuscito: memoria insufficiente");
        goto out;
    }
    settings_get_into(cfg);
    add_common(r);
    cJSON_AddNumberToObject(r, "issued", (double) s_lic.issued);
    // Funzioni in uso, per il confronto con la licenza sul server (§8.4).
    cJSON *use = cJSON_AddArrayToObject(r, "in_use");
    cJSON_AddItemToArray(use, cJSON_CreateString("rtk"));
    if (cfg->nmea_rate_hz > 1) {
        cJSON_AddItemToArray(use, cJSON_CreateString("nmea_fast"));
    }
    if (cfg->rover_has_fallback) {
        cJSON_AddItemToArray(use, cJSON_CreateString("has"));
    }
    if (cfg->comnav_ins_enable) {
        cJSON_AddItemToArray(use, cJSON_CreateString("ins"));
    }
    if (cfg->ntrip_caster_server_enable || cfg->vpn_enable || cfg->remote_interval_min || cfg->alert_enable) {
        cJSON_AddItemToArray(use, cJSON_CreateString("base_pro"));
    }
    char *json = cJSON_PrintUnformatted(r);
    if (!json) {
        goto out;
    }
    int st = post_signed("renew", json, resp, HTTP_BUF);
    free(json);
    int64_t now = time_ok() ? (int64_t) time(NULL) : 0;
    if (st == 304) {
        store_contact(now);
        set_msg("Licenza controllata: nessuna novita'");
    } else if (st == 200) {
        char why[96];
        if (accept_license(resp, why, sizeof(why))) {
            store_contact(now);
            set_msg(s_lic.revoked ? "Licenza aggiornata: ricevitore REVOCATO dal server (vale dal riavvio)"
                                  : "Licenza aggiornata (le novita' valgono dal riavvio)");
        } else {
            char m[128];
            snprintf(m, sizeof(m), "Rinnovo non riuscito: %s", why);
            set_msg(m);
        }
    } else {
        server_error_msg(st, resp, "Rinnovo");
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

static bool start_job(job_t *job, char *err, size_t err_size)
{
    if (s_busy) {
        snprintf(err, err_size, "operazione gia' in corso");
        free(job);
        return false;
    }
    s_busy = true;
    s_last_attempt_us = esp_timer_get_time();
    if (xTaskCreate(job_task, "license", TASK_STACK, job, 3, NULL) != pdPASS) {
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
    // Funzioni di questo avvio. Senza un'ora valida nessuna scadenza conta.
    bool tv = time_ok();
    int64_t now = tv ? (int64_t) time(NULL) : 0;
    for (int f = 0; f < LIC_COUNT; f++) {
        int64_t e = s_lic.expiry[f];
        s_granted_boot[f] = s_lic.valid && !s_lic.revoked && e >= 0 && (e == 0 || !tv || e > now);
    }
    if (!s_lic.valid) {
        ESP_LOGW(TAG, "Ricevitore NON attivato (%s)%s", why, LICENSE_ENFORCE ? "" : " - modalita' di prova: nessun blocco");
    } else {
        ESP_LOGI(TAG, "Licenza valida: cliente %s, %s, funzioni rtk=%d nmea_fast=%d has=%d ins=%d base_pro=%d%s",
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
    app_settings_t *cfg = heap_caps_malloc(sizeof(app_settings_t), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
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
    if (done && xTaskCreate(boot_task, "license_boot", TASK_STACK, done, 5, NULL) == pdPASS) {
        xSemaphoreTake(done, pdMS_TO_TICKS(15000));
    }
    if (done) {
        vSemaphoreDelete(done);
    }
}

// --- interrogazioni --------------------------------------------------------------------
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
        for (int f = 1; f < LIC_COUNT; f++) {
            int64_t e = l.expiry[f];
            if (e > 0 && e <= now) {
                out->state = LIC_STATE_EXTRA_EXPIRED;
                return;
            }
            if (e > 0 && e - now < EXPIRING_S) {
                out->state = LIC_STATE_EXPIRING;
            }
        }
    }
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
    if (status_get_net() == NET_STATUS_NONE) {
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
    if (!s_lic.valid) {
        snprintf(err, err_size, "ricevitore non ancora attivato");
        return false;
    }
    if (status_get_net() == NET_STATUS_NONE) {
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
    if (s_busy || !s_lic.valid || !time_ok() || status_get_net() == NET_STATUS_NONE) {
        return;
    }
    int64_t now = (int64_t) time(NULL);
    bool due = now >= s_lic.renew_after || now - s_last_contact > 86400;
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
    if (url[0] && strncmp(url, "https://", 8) != 0 && strncmp(url, "http://", 7) != 0) {
        snprintf(err, err_size, "indirizzo non valido");
        return false;
    }
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
