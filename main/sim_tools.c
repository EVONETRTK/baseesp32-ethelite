#include "sim_tools.h"
#include "cellular_link.h"
#include "settings.h"
#include "alerts.h"

#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "esp_timer.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static const char *TAG = "sim_tools";

#define NVS_NS              "simcred"
#define SMS_BUF_SIZE        2048
#define SMS_KEEP_US         (10LL * 60 * 1000000)  // SMS letti tenuti in memoria 10 minuti
#define SMS_REPLY_WAIT_US   (60LL * 1000000)       // credito via SMS: risposta letta dopo 60 s

// Spinlock inizializzato alla compilazione: valido anche se il pannello
// chiede qualcosa prima di sim_tools_start(). Dentro solo copie brevi.
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static sim_tools_status_t s_st;

// Richiesta in attesa (dal pannello) e suoi parametri.
static volatile sim_action_t s_pending;
static char s_arg_a[32];
static char s_arg_b[96];

// SMS ricevuti: allocati solo quando servono, liberati dopo 10 minuti.
static char *s_sms;
static int64_t s_sms_at_us;

// Credito chiesto via SMS: risposta da leggere dopo un minuto.
static int64_t s_sms_reply_due_us;
static bool s_sms_reply_auto;

// Statici: le stringhe del modem sono lunghe e lo stack del task e' condiviso.
static char s_resp[512];
static const app_settings_t *s_cfg; // impostazioni del task degli avvisi, valide durante sim_tools_tick()

static void lock(void)
{
    portENTER_CRITICAL(&s_lock);
}

static void unlock(void)
{
    portEXIT_CRITICAL(&s_lock);
}

static void set_msg(const char *fmt, const char *arg)
{
    char tmp[sizeof(s_st.last_msg)];
    snprintf(tmp, sizeof(tmp), fmt, arg ? arg : ""); // fuori dalla sezione critica
    lock();
    memcpy(s_st.last_msg, tmp, sizeof(tmp));
    unlock();
}

// --- lettura dell'importo --------------------------------------------------

static bool window_has(const char *lo, int from, int to, const char *word)
{
    int n = (int) strlen(lo);
    if (from < 0) from = 0;
    if (to > n) to = n;
    int wl = (int) strlen(word);
    for (int i = from; i + wl <= to; i++) {
        if (memcmp(lo + i, word, (size_t) wl) == 0) {
            return true;
        }
    }
    return false;
}

static bool window_has_euro(const char *lo, int from, int to)
{
    return window_has(lo, from, to, "eur") || window_has(lo, from, to, "\xe2\x82\xac");
}

bool sim_tools_parse_euro(const char *text, float *eur)
{
    char lo[256];
    size_t n = 0;
    for (; text[n] && n < sizeof(lo) - 1; n++) {
        unsigned char c = (unsigned char) text[n];
        lo[n] = c < 128 ? (char) tolower(c) : (char) c;
    }
    lo[n] = '\0';

    int best_score = 0;
    float best = 0;
    for (int i = 0; i < (int) n; i++) {
        if (!isdigit((unsigned char) lo[i])) {
            continue;
        }
        // Solo l'inizio di un numero, e non dentro date, ore o codici.
        if (i > 0 && (isdigit((unsigned char) lo[i - 1]) || lo[i - 1] == '.' || lo[i - 1] == ',' ||
                      lo[i - 1] == '/' || lo[i - 1] == ':' || lo[i - 1] == '*' || lo[i - 1] == '#' ||
                      isalpha((unsigned char) lo[i - 1]))) {
            continue;
        }
        int j = i;
        while (isdigit((unsigned char) lo[j])) j++;
        int int_digits = j - i;
        int dec = 0;
        if ((lo[j] == '.' || lo[j] == ',') && isdigit((unsigned char) lo[j + 1])) {
            int k = j + 1;
            while (isdigit((unsigned char) lo[k])) k++;
            dec = k - j - 1;
            if (dec > 2) {
                i = k;
                continue; // migliaia o codici, non un importo
            }
            j = k;
        }
        if (int_digits > 4 || lo[j] == '/' || lo[j] == ':' || lo[j] == '%') {
            i = j;
            continue;
        }
        // GB, minuti, SMS, giorni: quantita', non euro.
        int a = j;
        while (lo[a] == ' ') a++;
        if (!strncmp(lo + a, "gb", 2) || !strncmp(lo + a, "mb", 2) || !strncmp(lo + a, "min", 3) ||
            !strncmp(lo + a, "sms", 3) || !strncmp(lo + a, "giorn", 5) || !strncmp(lo + a, "ore", 3)) {
            i = j;
            continue;
        }
        int score = 0;
        if (window_has_euro(lo, j, j + 8) || window_has_euro(lo, i - 9, i)) {
            score += 3;
        }
        if (window_has(lo, i - 40, i, "credit") || window_has(lo, i - 40, i, "saldo") ||
            window_has(lo, i - 40, i, "residu")) {
            score += 2;
        }
        if (dec == 2) {
            score += 1;
        }
        if (score >= 3 && score > best_score) {
            char num[16];
            int len = j - i < (int) sizeof(num) - 1 ? j - i : (int) sizeof(num) - 1;
            memcpy(num, lo + i, (size_t) len);
            num[len] = '\0';
            for (char *p = num; *p; p++) {
                if (*p == ',') *p = '.';
            }
            best = strtof(num, NULL);
            best_score = score;
        }
        i = j;
    }
    if (best_score > 0) {
        *eur = best;
        return true;
    }
    return false;
}

// --- risposte del modem ----------------------------------------------------

static int hexval(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    c = (char) tolower((unsigned char) c);
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    return -1;
}

// Alcuni operatori rispondono in UCS2 esadecimale ("0043007200..."):
// convertito in UTF-8. Lascia il testo com'e' se non lo sembra.
static void decode_ucs2_hex_inplace(char *s)
{
    size_t n = strlen(s);
    if (n < 8 || n % 4) {
        return;
    }
    for (size_t i = 0; i < n; i++) {
        if (hexval(s[i]) < 0) {
            return;
        }
    }
    size_t w = 0;
    for (size_t i = 0; i < n; i += 4) {
        unsigned cp = (unsigned) (hexval(s[i]) << 12 | hexval(s[i + 1]) << 8 | hexval(s[i + 2]) << 4 | hexval(s[i + 3]));
        if (cp < 0x80) {
            s[w++] = (char) cp;
        } else if (cp < 0x800) {
            s[w++] = (char) (0xC0 | cp >> 6);
            s[w++] = (char) (0x80 | (cp & 0x3F));
        } else {
            s[w++] = (char) (0xE0 | cp >> 12);
            s[w++] = (char) (0x80 | ((cp >> 6) & 0x3F));
            s[w++] = (char) (0x80 | (cp & 0x3F));
        }
    }
    s[w] = '\0';
}

// "+CUSD: 0,"testo",15" -> testo (sul posto).
static bool extract_ussd_text(char *resp)
{
    char *p = strstr(resp, "+CUSD:");
    char *q1 = p ? strchr(p, '"') : NULL;
    char *q2 = q1 ? strrchr(q1 + 1, '"') : NULL;
    if (!q1 || !q2) {
        return false;
    }
    size_t len = (size_t) (q2 - q1 - 1);
    memmove(resp, q1 + 1, len);
    resp[len] = '\0';
    decode_ucs2_hex_inplace(resp);
    return true;
}

// Testo dell'ultimo SMS ricevuto in una risposta di AT+CMGL (sul posto).
static bool extract_last_sms_text(char *resp)
{
    char *last = NULL, *p = resp;
    while ((p = strstr(p, "+CMGL:")) != NULL) {
        last = p;
        p += 6;
    }
    char *nl = last ? strchr(last, '\n') : NULL;
    if (!nl) {
        return false;
    }
    char *start = nl + 1;
    char *end = strstr(start, "\r\n");
    size_t len = end ? (size_t) (end - start) : strlen(start);
    memmove(resp, start, len);
    resp[len] = '\0';
    decode_ucs2_hex_inplace(resp);
    return len > 0;
}

// --- credito ---------------------------------------------------------------

static void store_credit(const char *text, bool automatic)
{
    float eur = 0;
    bool valid = sim_tools_parse_euro(text, &eur);
    time_t now = time(NULL);
    lock();
    s_st.credit_at = now;
    strncpy(s_st.credit_text, text, sizeof(s_st.credit_text) - 1);
    s_st.credit_text[sizeof(s_st.credit_text) - 1] = '\0';
    s_st.credit_eur_valid = valid;
    s_st.credit_eur = eur;
    unlock();

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "txt", s_st.credit_text);
        nvs_set_u32(h, "at", (uint32_t) now);
        int32_t cents = valid ? (int32_t) (eur * 100 + 0.5f) : -1;
        nvs_set_i32(h, "cent", cents);
        nvs_commit(h);
        nvs_close(h);
    }
    if (valid) {
        ESP_LOGI(TAG, "Credito letto: %.2f euro", eur);
    } else {
        ESP_LOGW(TAG, "Importo non riconosciuto nella risposta: %s", text);
    }

    // Avviso: sotto la soglia, oppure (controllo automatico) importo non letto.
    float min = s_cfg->sim_credit_min_eur;
    char body[256];
    if (valid && min > 0 && eur < min) {
        snprintf(body, sizeof(body), "EVONETRTK %s: credito della SIM %.2f euro, sotto la soglia di %.2f euro. Ricarica la SIM.",
                 s_cfg->device_serial, eur, min);
        alerts_send_now(s_cfg, "EVONETRTK - credito SIM basso", body);
    } else if (!valid && automatic) {
        snprintf(body, sizeof(body), "EVONETRTK %s: controllo del credito, importo non riconosciuto. Risposta: %.150s",
                 s_cfg->device_serial, text);
        alerts_send_now(s_cfg, "EVONETRTK - controllo credito SIM", body);
    }
}

static void do_credit(bool automatic)
{
    if (s_cfg->sim_credit_mode == 1) {
        if (!s_cfg->sim_credit_sms_number[0]) {
            set_msg("Manca il numero a cui mandare l'SMS del credito", NULL);
            return;
        }
        if (cellular_link_send_sms(s_cfg->sim_credit_sms_number, s_cfg->sim_credit_sms_text)) {
            s_sms_reply_due_us = esp_timer_get_time() + SMS_REPLY_WAIT_US;
            s_sms_reply_auto = automatic;
            set_msg("SMS del credito inviato a %s: la risposta viene letta tra un minuto", s_cfg->sim_credit_sms_number);
        } else {
            set_msg("Invio dell'SMS del credito fallito (modem, SIM o segnale)", NULL);
        }
        return;
    }
    if (!s_cfg->sim_credit_code[0]) {
        set_msg("Manca il codice USSD del credito: scegli l'operatore o scrivilo", NULL);
        return;
    }
    if (cellular_link_ussd(s_cfg->sim_credit_code, s_resp, sizeof(s_resp)) && extract_ussd_text(s_resp)) {
        store_credit(s_resp, automatic);
        set_msg("Credito letto con %s", s_cfg->sim_credit_code);
    } else {
        set_msg("Nessuna risposta al codice %s (codice sbagliato per l'operatore, o modem non pronto)", s_cfg->sim_credit_code);
    }
}

// --- richieste --------------------------------------------------------------

bool sim_tools_request(sim_action_t action, const char *a, const char *b, char *err, size_t err_size)
{
    lock();
    bool busy = s_st.busy || s_pending != SIM_ACT_NONE;
    if (!busy) {
        strncpy(s_arg_a, a ? a : "", sizeof(s_arg_a) - 1);
        s_arg_a[sizeof(s_arg_a) - 1] = '\0';
        strncpy(s_arg_b, b ? b : "", sizeof(s_arg_b) - 1);
        s_arg_b[sizeof(s_arg_b) - 1] = '\0';
        s_pending = action;
        strcpy(s_st.last_msg, "In corso...");
    }
    unlock();
    if (busy) {
        snprintf(err, err_size, "Un'altra operazione sulla SIM e' in corso, riprova tra poco");
        return false;
    }
    alerts_wake();
    return true;
}

static void run_action(sim_action_t act)
{
    lock();
    s_st.busy = true;
    unlock();

    if (!cellular_link_modem_present()) {
        set_msg("Modem non presente o non risponde: monta il SIM7600 e scegli il modulo nella scheda SIM", NULL);
    } else {
        switch (act) {
        case SIM_ACT_CREDIT:
            do_credit(false);
            break;
        case SIM_ACT_USSD:
            if (cellular_link_ussd(s_arg_a, s_resp, sizeof(s_resp)) && extract_ussd_text(s_resp)) {
                store_credit(s_resp, false);
                set_msg("Risposta al codice %s ricevuta", s_arg_a);
            } else {
                set_msg("Nessuna risposta al codice %s", s_arg_a);
            }
            break;
        case SIM_ACT_SMS:
            set_msg(cellular_link_send_sms(s_arg_a, s_arg_b) ? "SMS inviato a %s" : "Invio dell'SMS a %s fallito", s_arg_a);
            break;
        case SIM_ACT_READ_SMS:
            if (!s_sms) {
                s_sms = malloc(SMS_BUF_SIZE);
            }
            if (s_sms && cellular_link_read_sms(s_sms, SMS_BUF_SIZE)) {
                s_sms_at_us = esp_timer_get_time();
                set_msg("SMS letti", NULL);
            } else {
                set_msg("Lettura degli SMS fallita", NULL);
            }
            break;
        case SIM_ACT_DELETE_SMS:
            set_msg(cellular_link_delete_sms() ? "SMS cancellati dalla SIM" : "Cancellazione degli SMS fallita", NULL);
            if (s_sms) {
                s_sms[0] = '\0';
            }
            break;
        default:
            break;
        }
    }
    lock();
    s_st.busy = false;
    unlock();
}

void sim_tools_tick(const app_settings_t *cfg)
{
    s_cfg = cfg;
    int64_t now_us = esp_timer_get_time();

    sim_action_t act = s_pending;
    if (act != SIM_ACT_NONE) {
        run_action(act);
        s_pending = SIM_ACT_NONE;
    }

    // Risposta all'SMS del credito.
    if (s_sms_reply_due_us && now_us >= s_sms_reply_due_us) {
        s_sms_reply_due_us = 0;
        if (cellular_link_read_sms(s_resp, sizeof(s_resp)) && extract_last_sms_text(s_resp)) {
            store_credit(s_resp, s_sms_reply_auto);
            set_msg("Risposta all'SMS del credito letta", NULL);
        } else {
            set_msg("Nessuna risposta all'SMS del credito: guarda gli SMS ricevuti", NULL);
        }
    }

    // SMS letti: memoria liberata dopo 10 minuti.
    if (s_sms && now_us - s_sms_at_us > SMS_KEEP_US) {
        free(s_sms);
        s_sms = NULL;
    }

    // ICCID una volta, quando il modem risponde.
    if (!s_st.iccid[0] && cellular_link_modem_present()) {
        char iccid[24];
        if (cellular_link_get_iccid(iccid, sizeof(iccid))) {
            lock();
            strncpy(s_st.iccid, iccid, sizeof(s_st.iccid) - 1);
            unlock();
        }
    }

    // Controllo automatico: ogni N giorni, dall'ora scelta.
    time_t now = time(NULL);
    if (s_cfg->sim_credit_every_days == 0 || now < 1700000000) {
        return;
    }
    struct tm tm;
    localtime_r(&now, &tm);
    if (tm.tm_hour < s_cfg->sim_credit_hour) {
        return;
    }
    uint32_t day = (uint32_t) (now / 86400); // giorni dal 1970, per contare l'intervallo
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return;
    }
    uint32_t last = 0;
    nvs_get_u32(h, "auto", &last);
    if (last == 0 || day - last >= s_cfg->sim_credit_every_days) {
        nvs_set_u32(h, "auto", day);
        nvs_commit(h);
        nvs_close(h);
        if (cellular_link_modem_present()) {
            ESP_LOGI(TAG, "Controllo automatico del credito");
            lock();
            s_st.busy = true;
            unlock();
            do_credit(true);
            lock();
            s_st.busy = false;
            unlock();
        }
        return;
    }
    nvs_close(h);
}

void sim_tools_get_status(sim_tools_status_t *out)
{
    lock();
    *out = s_st;
    unlock();
}

void sim_tools_get_sms(char *out, size_t out_size)
{
    lock();
    if (s_sms) {
        strncpy(out, s_sms, out_size - 1);
        out[out_size - 1] = '\0';
    } else {
        out[0] = '\0';
    }
    unlock();
}

void sim_tools_start(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        size_t len = sizeof(s_st.credit_text);
        if (nvs_get_str(h, "txt", s_st.credit_text, &len) == ESP_OK) {
            uint32_t at = 0;
            int32_t cents = -1;
            nvs_get_u32(h, "at", &at);
            nvs_get_i32(h, "cent", &cents);
            s_st.credit_at = (time_t) at;
            s_st.credit_eur_valid = cents >= 0;
            s_st.credit_eur = cents / 100.0f;
        }
        nvs_close(h);
    }
}
