#include "alerts.h"
#include "settings.h"
#include "status.h"

#include <string.h>
#include <stdio.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "esp_tls.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "mbedtls/base64.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "alerts";

// Il task di monitoraggio controlla ogni minuto - soglia minima di senso
// pratico per non generare falsi allarmi su una disconnessione/riconnessione
// transitoria (es. un singolo pacchetto perso).
#define ALERT_CHECK_INTERVAL_MS (60 * 1000)

// Legge la risposta SMTP corrente (puo' essere su piu' righe, es. dopo
// EHLO: "250-STARTTLS\r\n250 AUTH LOGIN\r\n") in buf, fino a max_len.
// L'ultima riga di una risposta multi-riga e' quella con uno spazio (non
// un trattino) subito dopo le 3 cifre del codice - continua a leggere
// finche' non la trova. Ritorna true se il codice iniziale combacia con
// expected_code.
static bool smtp_read_response(esp_tls_t *tls, char *buf, size_t max_len, const char *expected_code)
{
    size_t used = 0;
    int empty_reads = 0;
    while (used + 1 < max_len && empty_reads < 20) {
        int r = esp_tls_conn_read(tls, buf + used, max_len - 1 - used);
        if (r < 0) {
            return false;
        }
        if (r == 0) {
            empty_reads++;
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }
        used += (size_t) r;
        buf[used] = '\0';
        if (used >= 2 && buf[used - 2] == '\r' && buf[used - 1] == '\n') {
            size_t line_start = used - 2;
            while (line_start > 0 && buf[line_start - 1] != '\n') {
                line_start--;
            }
            if (used - line_start >= 4 && buf[line_start + 3] == ' ') {
                break; // riga finale della risposta (non una continuazione multi-riga)
            }
        }
    }
    return used > 0 && strncmp(buf, expected_code, strlen(expected_code)) == 0;
}

static bool smtp_send(esp_tls_t *tls, const char *data, size_t len)
{
    size_t sent = 0;
    while (sent < len) {
        int r = esp_tls_conn_write(tls, data + sent, len - sent);
        if (r <= 0) {
            return false;
        }
        sent += (size_t) r;
    }
    return true;
}

// Client SMTP minimale (SMTPS, TLS implicito - tipicamente porta 465) con
// autenticazione AUTH LOGIN, sufficiente per i principali provider (Gmail
// con password per le app, Outlook, ecc.). Non implementa STARTTLS ne'
// altri meccanismi di autenticazione: qui basta e avanza per un avviso
// automatico punto-punto, non e' un client email generico.
static bool smtp_send_email(const char *host, uint16_t port, const char *user, const char *password,
                             const char *to, const char *subject, const char *body,
                             char *out_detail, size_t out_detail_size)
{
    esp_tls_t *tls = esp_tls_init();
    if (!tls) {
        if (out_detail) snprintf(out_detail, out_detail_size, "memoria esaurita");
        return false;
    }

    esp_tls_cfg_t cfg = {
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 10000,
    };
    if (esp_tls_conn_new_sync(host, (int) strlen(host), port, &cfg, tls) != 1) {
        esp_tls_conn_destroy(tls);
        if (out_detail) snprintf(out_detail, out_detail_size, "connessione al server SMTP fallita");
        return false;
    }

    bool ok = false;
    const char *step = "saluto server";
    char buf[512];
    char cmd[320];

    do {
        if (!smtp_read_response(tls, buf, sizeof(buf), "220")) break;

        step = "EHLO";
        int n = snprintf(cmd, sizeof(cmd), "EHLO evonetrtk\r\n");
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "250")) break;

        step = "AUTH LOGIN";
        n = snprintf(cmd, sizeof(cmd), "AUTH LOGIN\r\n");
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "334")) break;

        step = "invio utente";
        unsigned char b64[192];
        size_t b64_len = 0;
        mbedtls_base64_encode(b64, sizeof(b64) - 1, &b64_len, (const unsigned char *) user, strlen(user));
        b64[b64_len] = '\0';
        n = snprintf(cmd, sizeof(cmd), "%s\r\n", (char *) b64);
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "334")) break;

        step = "invio password (credenziali rifiutate?)";
        mbedtls_base64_encode(b64, sizeof(b64) - 1, &b64_len, (const unsigned char *) password, strlen(password));
        b64[b64_len] = '\0';
        n = snprintf(cmd, sizeof(cmd), "%s\r\n", (char *) b64);
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "235")) break;

        step = "MAIL FROM";
        n = snprintf(cmd, sizeof(cmd), "MAIL FROM:<%s>\r\n", user);
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "250")) break;

        step = "RCPT TO (destinatario rifiutato?)";
        n = snprintf(cmd, sizeof(cmd), "RCPT TO:<%s>\r\n", to);
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "250")) break;

        step = "DATA";
        n = snprintf(cmd, sizeof(cmd), "DATA\r\n");
        if (!smtp_send(tls, cmd, (size_t) n) || !smtp_read_response(tls, buf, sizeof(buf), "354")) break;

        step = "invio messaggio";
        char msg[768];
        int msg_len = snprintf(msg, sizeof(msg), "Subject: %s\r\nFrom: %s\r\nTo: %s\r\n\r\n%s\r\n.\r\n",
                                subject, user, to, body);
        if (!smtp_send(tls, msg, (size_t) msg_len) || !smtp_read_response(tls, buf, sizeof(buf), "250")) break;

        n = snprintf(cmd, sizeof(cmd), "QUIT\r\n");
        smtp_send(tls, cmd, (size_t) n); // esito non determinante, il messaggio e' gia' accettato

        ok = true;
    } while (0);

    if (!ok && out_detail) {
        snprintf(out_detail, out_detail_size, "fallito al passo \"%s\" (risposta server: %.60s)", step, buf);
    }

    esp_tls_conn_destroy(tls);
    return ok;
}

// Percent-encoding minimale per il parametro "text" della richiesta HTTP -
// solo i caratteri non riservati (RFC 3986) restano invariati.
static void url_encode(const char *in, char *out, size_t out_size)
{
    size_t o = 0;
    for (size_t i = 0; in[i] != '\0' && o + 4 < out_size; i++) {
        unsigned char c = (unsigned char) in[i];
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
            c == '-' || c == '_' || c == '.' || c == '~') {
            out[o++] = (char) c;
        } else {
            o += (size_t) snprintf(out + o, out_size - o, "%%%02X", c);
        }
    }
    out[o] = '\0';
}

// Invia un messaggio WhatsApp tramite CallMeBot (https://www.callmebot.com/),
// un servizio gratuito di terze parti che fa da ponte verso WhatsApp - il
// numero di telefono va prima registrato una tantum inviando un messaggio
// al bot ufficiale per ottenere la apikey personale. Non e' un canale
// ufficiale/garantito: puo' smettere di funzionare senza preavviso se il
// servizio cambia o chiude, per questo resta un'opzione aggiuntiva
// all'email, non l'unica.
static bool whatsapp_send_callmebot(const char *phone, const char *apikey, const char *text,
                                     char *out_detail, size_t out_detail_size)
{
    char encoded_text[300];
    url_encode(text, encoded_text, sizeof(encoded_text));

    char url[420];
    snprintf(url, sizeof(url), "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
             phone, encoded_text, apikey);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    bool ok = (err == ESP_OK && status >= 200 && status < 300);
    if (!ok && out_detail) {
        snprintf(out_detail, out_detail_size, "richiesta fallita (err=%s status=%d)", esp_err_to_name(err), status);
    }
    return ok;
}

// Prova ad inviare su ogni canale configurato (un campo host/telefono
// vuoto = canale disattivato, nessun errore). out_msg (se non NULL)
// riceve un riepilogo di entrambi gli esiti. Ritorna true se almeno un
// canale configurato ha avuto successo, false se tutti quelli configurati
// sono falliti o nessuno e' configurato.
static bool send_on_configured_channels(const app_settings_t *s, const char *subject, const char *body,
                                         char *out_msg, size_t out_msg_size)
{
    bool any_configured = false;
    bool any_ok = false;
    char summary[256] = {0};

    if (s->alert_smtp_host[0] && s->alert_email_to[0]) {
        any_configured = true;
        char detail[100] = {0};
        bool ok = smtp_send_email(s->alert_smtp_host, s->alert_smtp_port, s->alert_smtp_user,
                                   s->alert_smtp_password, s->alert_email_to, subject, body,
                                   detail, sizeof(detail));
        ESP_LOGI(TAG, "Invio email: %s%s%s", ok ? "riuscito" : "fallito", ok ? "" : " - ", ok ? "" : detail);
        any_ok |= ok;
        char line[128];
        snprintf(line, sizeof(line), "Email: %s%s%s. ", ok ? "riuscita" : "fallita", ok ? "" : " - ", ok ? "" : detail);
        strncat(summary, line, sizeof(summary) - strlen(summary) - 1);
    }

    if (s->alert_whatsapp_phone[0] && s->alert_whatsapp_apikey[0]) {
        any_configured = true;
        char detail[100] = {0};
        bool ok = whatsapp_send_callmebot(s->alert_whatsapp_phone, s->alert_whatsapp_apikey, body,
                                           detail, sizeof(detail));
        ESP_LOGI(TAG, "Invio WhatsApp: %s%s%s", ok ? "riuscito" : "fallito", ok ? "" : " - ", ok ? "" : detail);
        any_ok |= ok;
        char line[128];
        snprintf(line, sizeof(line), "WhatsApp: %s%s%s.", ok ? "riuscito" : "fallito", ok ? "" : " - ", ok ? "" : detail);
        strncat(summary, line, sizeof(summary) - strlen(summary) - 1);
    }

    if (!any_configured) {
        snprintf(summary, sizeof(summary), "Nessun canale configurato (email o WhatsApp)");
    }
    if (out_msg) {
        strncpy(out_msg, summary, out_msg_size - 1);
        out_msg[out_msg_size - 1] = '\0';
    }
    return any_ok;
}

bool alerts_send_test(const app_settings_t *s, char *out_msg, size_t out_msg_size)
{
    char body[128];
    snprintf(body, sizeof(body), "EVONETRTK %s: avviso di prova, se lo ricevi la configurazione funziona.",
             s->device_serial);
    return send_on_configured_channels(s, "EVONETRTK - avviso di prova", body, out_msg, out_msg_size);
}

static void alerts_task(void *arg)
{
    bool already_alerted = false;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(ALERT_CHECK_INTERVAL_MS));

        app_settings_t s = settings_get();
        if (!s.alert_enable) {
            already_alerted = false;
            continue;
        }

        ntrip_conn_status_t ntrip = status_ntrip_get();
        if (!ntrip.connected) {
            if (ntrip.last_disconnect_us <= 0) {
                continue; // mai stato connesso dal boot: non e' una disconnessione da segnalare
            }
            int down_min = (int) ((esp_timer_get_time() - ntrip.last_disconnect_us) / 1000000 / 60);
            if (!already_alerted && down_min >= s.alert_threshold_min) {
                char body[256];
                snprintf(body, sizeof(body),
                         "EVONETRTK %s: connessione al caster NTRIP interrotta da oltre %d minuti (%s)",
                         s.device_serial, s.alert_threshold_min, ntrip.last_error);
                send_on_configured_channels(&s, "EVONETRTK - caster disconnesso", body, NULL, 0);
                already_alerted = true;
            }
        } else if (already_alerted) {
            char body[128];
            snprintf(body, sizeof(body), "EVONETRTK %s: connessione al caster NTRIP ripristinata.", s.device_serial);
            send_on_configured_channels(&s, "EVONETRTK - caster ripristinato", body, NULL, 0);
            already_alerted = false;
        }
    }
}

void alerts_start(void)
{
    xTaskCreate(alerts_task, "alerts", 10240, NULL, 3, NULL);
}
