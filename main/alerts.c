#include "alerts.h"
#include "license.h"
#include "settings.h"
#include "status.h"
#include "base_monitor.h"
#include "base_selfpos.h"
#include "sys_stats.h"
#include "data_usage.h"
#include "sim_plan.h"
#include "sim_tools.h"
#include "remote_status.h"
#include "time_sync.h"

#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>

#include "nvs.h"

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
    // Un a capo in utente o destinatario aggiungerebbe comandi SMTP (il
    // pannello li rifiuta gia' al salvataggio; qui per le configurazioni
    // salvate prima o importate).
    if (strpbrk(user, "\r\n") || strpbrk(to, "\r\n")) {
        if (out_detail) snprintf(out_detail, out_detail_size, "utente o destinatario non validi (a capo)");
        return false;
    }
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
    char buf[512] = ""; // nel messaggio d'errore anche se il server non ha risposto nulla
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

// Percent-encoding minimale per i parametri della richiesta HTTP - solo i
// caratteri non riservati (RFC 3986) restano invariati. Ogni carattere puo'
// diventare 3 byte: per non troncare, out_size = 3 * lunghezza + 4.
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
// Testo fino a WA_TEXT_MAX caratteri (i messaggi piu' lunghi sono ~320):
// prima il testo codificato stava in 300 byte e gli avvisi lunghi arrivavano
// tagliati (es. "verifica l'antenna" spariva). Buffer allocati, non sullo
// stack del task degli avvisi o del server web.
#define WA_TEXT_MAX 400

static bool whatsapp_send_callmebot(const char *phone, const char *apikey, const char *text,
                                     char *out_detail, size_t out_detail_size)
{
    // Numero: solo "+" iniziale e cifre (spazi, trattini, punti tolti). Un
    // numero scritto "+39 333 1234567" rendeva l'URL non valido: il client
    // HTTP non veniva creato e la base andava in crash (revisione del
    // 09/10/2026).
    char clean_phone[24];
    size_t n = 0;
    for (const char *c = phone; *c && n < sizeof(clean_phone) - 1; c++) {
        if ((*c >= '0' && *c <= '9') || (*c == '+' && n == 0)) {
            clean_phone[n++] = *c;
        }
    }
    clean_phone[n] = '\0';

    size_t text_len = strnlen(text, WA_TEXT_MAX);
    size_t enc_size = 3 * text_len + 4;
    size_t url_size = enc_size + 3 * sizeof(clean_phone) + 3 * strlen(apikey) + 96;
    char *encoded_text = malloc(enc_size);
    char *enc_phone = malloc(3 * sizeof(clean_phone) + 4);
    char *enc_key = malloc(3 * strlen(apikey) + 4);
    char *url = malloc(url_size);
    bool ok = false;
    if (!encoded_text || !enc_phone || !enc_key || !url) {
        if (out_detail) snprintf(out_detail, out_detail_size, "memoria insufficiente");
        goto done;
    }
    url_encode(text, encoded_text, enc_size); // si ferma da solo a buffer pieno
    url_encode(clean_phone, enc_phone, 3 * sizeof(clean_phone) + 4);
    url_encode(apikey, enc_key, 3 * strlen(apikey) + 4);
    snprintf(url, url_size, "https://api.callmebot.com/whatsapp.php?phone=%s&text=%s&apikey=%s",
             enc_phone, encoded_text, enc_key);

    esp_http_client_config_t config = {
        .url = url,
        .crt_bundle_attach = esp_crt_bundle_attach,
        .timeout_ms = 15000,
    };
    esp_http_client_handle_t client = esp_http_client_init(&config);
    if (!client) {
        // URL rifiutato dal client HTTP o memoria esaurita: prima si
        // proseguiva con un puntatore NULL (crash).
        if (out_detail) snprintf(out_detail, out_detail_size, "numero o API key non validi, o memoria insufficiente");
        goto done;
    }
    esp_err_t err = esp_http_client_perform(client);
    int status = esp_http_client_get_status_code(client);
    esp_http_client_cleanup(client);

    ok = (err == ESP_OK && status >= 200 && status < 300);
    if (!ok && out_detail) {
        snprintf(out_detail, out_detail_size, "richiesta fallita (err=%s status=%d)", esp_err_to_name(err), status);
    }
done:
    free(encoded_text);
    free(enc_phone);
    free(enc_key);
    free(url);
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

bool alerts_send_now(const app_settings_t *s, const char *subject, const char *body)
{
    return send_on_configured_channels(s, subject, body, NULL, 0);
}

bool alerts_send_test(const app_settings_t *s, char *out_msg, size_t out_msg_size)
{
    char body[128];
    snprintf(body, sizeof(body), "EVONETRTK %s: avviso di prova, se lo ricevi la configurazione funziona.",
             s->device_serial);
    return send_on_configured_channels(s, "EVONETRTK - avviso di prova", body, out_msg, out_msg_size);
}

static TaskHandle_t s_alerts_task;

void alerts_wake(void)
{
    if (s_alerts_task) {
        xTaskNotifyGive(s_alerts_task);
    }
}

// Promemoria della licenza (docs/attivazione-licenze.md §1): le funzioni
// extra scadono tra 30 e tra 7 giorni. Uno per soglia, ricordato nella NVS
// (scadenza e soglia) per non ripeterlo a ogni riavvio; sui canali gia'
// configurati per gli avvisi, anche con gli avvisi spenti (come la SIM).
static void license_reminder_tick(const app_settings_t *s)
{
    int64_t exp = 0;
    if (!time_sync_is_valid() || time(NULL) < 1700000000 || !license_next_extra_expiry(&exp)) {
        return;
    }
    int64_t left = exp - (int64_t) time(NULL);
    uint8_t stage = left <= 7 * 86400 ? 7 : (left <= 30 * 86400 ? 30 : 0);
    if (left <= 0 || stage == 0) {
        return; // dopo la scadenza lo dice il pannello (si spengono al riavvio)
    }
    bool any_channel = (s->alert_smtp_host[0] && s->alert_email_to[0]) ||
                       (s->alert_whatsapp_phone[0] && s->alert_whatsapp_apikey[0]);
    if (!any_channel) {
        return;
    }
    nvs_handle_t h;
    if (nvs_open("alerts", NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    int64_t done_exp = 0;
    uint8_t done_stage = 0;
    nvs_get_i64(h, "licexp", &done_exp);
    nvs_get_u8(h, "licstg", &done_stage);
    // Gia' avvisato per questa scadenza a questa soglia (o a quella dei 7 giorni).
    if (done_exp == exp && done_stage != 0 && done_stage <= stage) {
        nvs_close(h);
        return;
    }
    time_t t = (time_t) exp;
    struct tm tm;
    localtime_r(&t, &tm);
    int days = (int) ((left + 86399) / 86400);
    char body[300];
    snprintf(body, sizeof(body),
             "EVONETRTK %s: le funzioni extra della licenza (autosterzo 5-10 Hz, riserva HAS, IMU/doppia antenna) "
             "scadono il %02d/%02d/%04d (tra %d giorni). Rinnovale dal portale EVONETRTK: dopo la scadenza si "
             "spengono al riavvio successivo.",
             s->device_serial, tm.tm_mday, tm.tm_mon + 1, tm.tm_year + 1900, days);
    bool ok = alerts_send_now(s, "EVONETRTK - licenza in scadenza", body);
    ESP_LOGW(TAG, "Promemoria scadenza licenza (%d giorni) %s", days, ok ? "inviato" : "NON inviato (canali in errore)");
    // Salvato anche se l'invio fallisce: niente tentativi ogni minuto.
    nvs_set_i64(h, "licexp", exp);
    nvs_set_u8(h, "licstg", stage);
    nvs_commit(h);
    nvs_close(h);
}

static void alerts_task(void *arg)
{
    bool already_alerted = false;
    bool already_alerted_drift = false;
    bool already_alerted_temp = false;
    bool already_alerted_1005 = false;
    static char alerted_data_month[16]; // mese gia' avvisato per il piano dati
    while (1) {
        // Ogni minuto, o subito se il pannello chiede un'operazione sulla SIM
        // (alerts_wake(), sim_tools.c).
        ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(ALERT_CHECK_INTERVAL_MS));
        sys_stats_heartbeat(HB_ALERTS);

        // Copia statica (2,4 KB), non sullo stack: con la catena email/TLS il
        // task arrivava a ~10,2 KB su 10,24 (revisione del 09/10/2026). Usata
        // solo da questo task.
        static app_settings_t s;
        settings_get_into(&s);
        sim_plan_tick(&s);  // rinnovo della SIM: promemoria anche con gli altri avvisi spenti
        sim_tools_tick(&s); // credito, SMS (anche con gli altri avvisi spenti)
        license_tick();         // rinnovo della licenza quando e' il momento
        license_reminder_tick(&s); // extra in scadenza: promemoria a 30 e 7 giorni, come quelli della SIM
        bool base_pkg = license_has(LIC_BASE);
        if (base_pkg) {
            remote_status_tick(&s); // monitoraggio remoto, se impostato
        }
        // Piano dati della SIM, per tutti anche con gli altri avvisi spenti:
        // un avviso al mese quando il traffico stimato
        // sul cellulare supera l'80% del piano impostato nel pannello.
        if (s.data_plan_mb > 0) {
            data_usage_t du = data_usage_get();
            if (du.month[0] && strcmp(du.month, alerted_data_month) != 0 &&
                du.cell_month >= (uint64_t) s.data_plan_mb * 1000000ULL * 8 / 10) {
                char body[200];
                snprintf(body, sizeof(body),
                         "EVONETRTK %s: traffico dati sulla SIM a %.0f MB su %u MB del piano (stima, mese %s).",
                         s.device_serial, du.cell_month / 1e6, (unsigned) s.data_plan_mb, du.month);
                send_on_configured_channels(&s, "EVONETRTK - piano dati quasi esaurito", body, NULL, 0);
                strncpy(alerted_data_month, du.month, sizeof(alerted_data_month) - 1);
            }
        }

        if (!s.alert_enable) {
            already_alerted = false;
            already_alerted_drift = false;
            already_alerted_temp = false;
            already_alerted_1005 = false;
            continue;
        }
        // Senza Pacchetto Base restano gli avvisi per tutti (1005, piano dati
        // della SIM, temperatura, che riguarda la sicurezza; promemoria SIM e
        // credito sono sopra); caster scollegato e antenna spostata fanno parte
        // del Pacchetto Base.

        ntrip_conn_status_t ntrip = status_ntrip_get();
        if (!base_pkg) {
            already_alerted = false;
        } else if (!ntrip.connected) {
            if (ntrip.last_disconnect_us <= 0) {
                // mai stato connesso dal boot: non e' una disconnessione da segnalare
            } else {
                int down_min = (int) ((esp_timer_get_time() - ntrip.last_disconnect_us) / 1000000 / 60);
                if (!already_alerted && down_min >= s.alert_threshold_min) {
                    char body[256];
                    snprintf(body, sizeof(body),
                             "EVONETRTK %s: connessione al caster NTRIP interrotta da oltre %d minuti (%s)",
                             s.device_serial, s.alert_threshold_min, ntrip.last_error);
                    send_on_configured_channels(&s, "EVONETRTK - caster disconnesso", body, NULL, 0);
                    already_alerted = true;
                }
            }
        } else if (already_alerted) {
            char body[128];
            snprintf(body, sizeof(body), "EVONETRTK %s: connessione al caster NTRIP ripristinata.", s.device_serial);
            send_on_configured_channels(&s, "EVONETRTK - caster ripristinato", body, NULL, 0);
            already_alerted = false;
        }

        // Spostamento dell'antenna (vedi base_selfpos.c: posizione ricalcolata
        // dai dati grezzi contro quella trasmessa, confermato su due finestre
        // di 30 minuti): stesso schema di "un avviso quando succede, uno
        // quando rientra" di sopra, con un latch separato.
        if (s.base_drift_alert_enable && base_pkg) {
            base_selfpos_status_t sp;
            base_selfpos_get_status(&sp);
            if (sp.alarm) {
                if (!already_alerted_drift) {
                    char body[320];
                    snprintf(body, sizeof(body),
                             "EVONETRTK %s: l'antenna della base sembra spostata di circa %.1f m in orizzontale "
                             "e %.1f m in verticale rispetto alla posizione che trasmette ai rover (controllo di 1 ora). "
                             "Le correzioni potrebbero essere sbagliate: verifica l'antenna.",
                             s.device_serial, sp.horiz_m, sp.d_up_m);
                    send_on_configured_channels(&s, "EVONETRTK - antenna della base spostata", body, NULL, 0);
                    already_alerted_drift = true;
                }
            } else if (already_alerted_drift && sp.have_result) {
                char body[160];
                snprintf(body, sizeof(body),
                         "EVONETRTK %s: la posizione dell'antenna della base e' di nuovo coerente (%.1f m).",
                         s.device_serial, sp.horiz_m);
                send_on_configured_channels(&s, "EVONETRTK - antenna della base a posto", body, NULL, 0);
                already_alerted_drift = false;
            }
        } else {
            already_alerted_drift = false;
        }

        // Base senza posizione (1005/1006): i rover non possono fare RTK, ma
        // tutto il resto sembra funzionare (caster collegato, dati in uscita).
        // Soglia: 30 minuti, o i minuti di disconnessione se piu' alti, per
        // non avvisare durante un normale survey-in all'avvio.
        if (!status_get_active_rover()) {
            base_monitor_status_t bm = base_monitor_get_status();
            int64_t now = esp_timer_get_time();
            int64_t since_us = bm.last_position_us > 0 ? now - bm.last_position_us : now;
            int limit_min = s.alert_threshold_min > 30 ? s.alert_threshold_min : 30;
            if (!already_alerted_1005 && since_us >= (int64_t) limit_min * 60 * 1000000) {
                char body[224];
                snprintf(body, sizeof(body),
                         "EVONETRTK %s: la base non invia la sua posizione (RTCM 1005) da oltre %d minuti, "
                         "i rover non possono fare RTK. Di solito: survey-in non completato (cielo coperto).",
                         s.device_serial, limit_min);
                send_on_configured_channels(&s, "EVONETRTK - base senza posizione", body, NULL, 0);
                already_alerted_1005 = true;
            } else if (already_alerted_1005 && bm.last_position_us > 0 && now - bm.last_position_us < 60 * 1000000) {
                char body[128];
                snprintf(body, sizeof(body), "EVONETRTK %s: la base invia di nuovo la sua posizione (1005).", s.device_serial);
                send_on_configured_channels(&s, "EVONETRTK - posizione base ripristinata", body, NULL, 0);
                already_alerted_1005 = false;
            }
        }

        // Temperatura del chip (letta da sys_stats.c): un avviso oltre la
        // soglia, uno quando torna sotto la soglia di rientro.
        sys_stats_t st = sys_stats_get();
        if (st.chip_temp_c > -1000) {
            if (!already_alerted_temp && st.chip_temp_c >= SYS_TEMP_ALERT_C) {
                char body[192];
                snprintf(body, sizeof(body),
                         "EVONETRTK %s: temperatura del chip a %.1f C (soglia %.0f C). "
                         "Controlla sole diretto o ventilazione della custodia.",
                         s.device_serial, (double) st.chip_temp_c, (double) SYS_TEMP_ALERT_C);
                send_on_configured_channels(&s, "EVONETRTK - temperatura alta", body, NULL, 0);
                already_alerted_temp = true;
            } else if (already_alerted_temp && st.chip_temp_c < SYS_TEMP_REARM_C) {
                char body[160];
                snprintf(body, sizeof(body), "EVONETRTK %s: temperatura del chip rientrata a %.1f C (massima %.1f C).",
                         s.device_serial, (double) st.chip_temp_c, (double) st.chip_temp_max_c);
                send_on_configured_channels(&s, "EVONETRTK - temperatura rientrata", body, NULL, 0);
                already_alerted_temp = false;
            }
        }
    }
}

void alerts_start(void)
{
    xTaskCreate(alerts_task, "alerts", 10240, NULL, 3, &s_alerts_task);
}
