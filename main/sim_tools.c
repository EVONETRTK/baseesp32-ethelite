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
    // Senza ora valida (prima di NTP/satelliti) niente data: il pannello
    // mostrava "01/01/1970". 0 = data della lettura sconosciuta.
    if (now < 1700000000) {
        now = 0;
    }
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

// --- ICCID, IMEI e numero ---------------------------------------------------

// ICCID per i confronti: solo cifre, senza la F finale di alcune SIM.
static void iccid_norm(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    for (; *in && n < out_size - 1; in++) {
        char c = (char) toupper((unsigned char) *in);
        if ((c >= '0' && c <= '9') || c == 'F') {
            out[n++] = c;
        }
    }
    while (n > 0 && out[n - 1] == 'F') n--;
    out[n] = '\0';
}

// Numero compatto per il modem: + iniziale e cifre.
static void number_compact(const char *in, char *out, size_t out_size)
{
    size_t n = 0;
    for (; *in && n < out_size - 1; in++) {
        if ((*in >= '0' && *in <= '9') || (*in == '+' && n == 0)) {
            out[n++] = *in;
        }
    }
    out[n] = '\0';
}

// Stesso numero con o senza prefisso internazionale: ultime 9 cifre.
static bool same_number(const char *a, const char *b)
{
    char da[20], db[20];
    size_t la = 0, lb = 0;
    for (; *a && la < sizeof(da) - 1; a++) if (*a >= '0' && *a <= '9') da[la++] = *a;
    for (; *b && lb < sizeof(db) - 1; b++) if (*b >= '0' && *b <= '9') db[lb++] = *b;
    if (la < 6 || lb < 6) {
        return false;
    }
    size_t k = la < lb ? la : lb;
    if (k > 9) k = 9;
    return memcmp(da + la - k, db + lb - k, k) == 0;
}

// Ultimi ICCID e IMEI letti, per mostrarli anche con il modem spento e per
// accorgersi all'avvio di una SIM o di un modem sostituiti.
static void save_ids(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_str(h, "iccid", s_st.iccid);
        nvs_set_str(h, "imei", s_st.imei);
        nvs_set_u32(h, "ids_at", (uint32_t) s_st.ids_at);
        nvs_commit(h);
        nvs_close(h);
    }
}

// Lettura di IMEI, ICCID e numero una volta per avvio, quando il modem
// risponde (l'ICCID solo con la SIM inserita; il numero puo' mancare nella
// SIM: risposta vuota = finito). Poi, se la SIM non contiene il numero, ci
// copia quello scritto a mano nel pannello.
static void read_ids(void)
{
    static bool s_imei_done, s_iccid_done, s_number_done;
    static char s_write_tried[20]; // numero gia' provato a scrivere nella SIM
    static char a[24], b[24];      // statici: lo stack del task e' condiviso

    if ((!s_imei_done || !s_iccid_done || !s_number_done) && cellular_link_modem_present()) {
        char val[24];
        bool got = false;
        if (!s_imei_done && cellular_link_get_imei(val, sizeof(val))) {
            s_imei_done = got = true;
            bool changed = s_st.imei[0] && strcmp(s_st.imei, val) != 0;
            if (changed) {
                ESP_LOGW(TAG, "Modem sostituito: IMEI %s, prima %s", val, s_st.imei);
                snprintf(s_resp, sizeof(s_resp), "EVONETRTK %s: modem sostituito. IMEI nuovo %s, prima %s.",
                         s_cfg->device_serial, val, s_st.imei);
            }
            lock();
            if (changed) {
                strlcpy(s_st.imei_prev, s_st.imei, sizeof(s_st.imei_prev));
            }
            strlcpy(s_st.imei, val, sizeof(s_st.imei));
            s_st.imei_live = true;
            unlock();
            if (changed) {
                alerts_send_now(s_cfg, "EVONETRTK - modem sostituito", s_resp);
            }
        }
        if (!s_iccid_done && cellular_link_get_iccid(val, sizeof(val))) {
            s_iccid_done = got = true;
            // Riferimento: l'ultimo letto; se mai letto, quello scritto a mano.
            const char *ref = s_st.iccid[0] ? s_st.iccid : s_cfg->sim_iccid_cfg;
            iccid_norm(val, a, sizeof(a));
            iccid_norm(ref, b, sizeof(b));
            bool changed = b[0] && strcmp(a, b) != 0;
            if (changed) {
                ESP_LOGW(TAG, "SIM cambiata: ICCID %s, prima %s", val, ref);
                snprintf(s_resp, sizeof(s_resp), "EVONETRTK %s: SIM dati cambiata. ICCID nuovo %s, prima %s. Aggiorna il numero di telefono nel pannello.",
                         s_cfg->device_serial, val, ref);
            }
            lock();
            if (changed) {
                strlcpy(s_st.iccid_prev, ref, sizeof(s_st.iccid_prev));
            }
            strlcpy(s_st.iccid, val, sizeof(s_st.iccid));
            s_st.iccid_live = true;
            unlock();
            if (changed) {
                alerts_send_now(s_cfg, "EVONETRTK - SIM cambiata", s_resp);
            }
        }
        if (got) {
            time_t now = time(NULL);
            if (now > 1700000000) { // ora gia' valida (NTP o satelliti)
                lock();
                s_st.ids_at = now;
                unlock();
            }
            save_ids();
        }
        char phone[20];
        if (!s_number_done && cellular_link_get_number(phone, sizeof(phone))) {
            s_number_done = true;
            lock();
            strlcpy(s_st.phone, phone, sizeof(s_st.phone));
            unlock();
        }
    }

    // Numero scritto a mano copiato nella SIM se la SIM non ne contiene
    // uno: una volta per numero e per avvio. Mai con una SIM appena cambiata
    // (il numero a mano sarebbe quello della SIM di prima), mai sopra un
    // numero che la SIM ha gia'.
    if (s_number_done && s_iccid_done && !s_st.phone[0] && !s_st.iccid_prev[0] && s_cfg->sim_phone[0]) {
        number_compact(s_cfg->sim_phone, a, sizeof(a));
        if (strlen(a) >= 6 && strcmp(a, s_write_tried) != 0) {
            strlcpy(s_write_tried, a, sizeof(s_write_tried));
            char phone[20];
            if (cellular_link_write_number(a) && cellular_link_get_number(phone, sizeof(phone)) && same_number(phone, a)) {
                lock();
                strlcpy(s_st.phone, phone, sizeof(s_st.phone));
                s_st.number_written = true;
                unlock();
                ESP_LOGI(TAG, "Numero %s scritto nella SIM", a);
                set_msg("Numero %s scritto nella SIM: ora la SIM lo conosce anche in un'altra base", a);
            } else {
                ESP_LOGW(TAG, "La SIM non accetta la scrittura del numero %s", a);
                set_msg("La SIM non accetta la scrittura del numero %s: resta salvato solo nella base", a);
            }
        }
    }
}

// --- richieste --------------------------------------------------------------

// Difesa anche qui, oltre ai controlli del pannello (e a quelli di
// cellular_link.c): codici e numeri finiscono dentro comandi AT.
static bool ussd_ok(const char *s)
{
    size_t n = 0;
    for (; s && *s; s++, n++) {
        if (!((*s >= '0' && *s <= '9') || *s == '*' || *s == '#' || *s == '+')) {
            return false;
        }
    }
    return n > 0 && n <= 40;
}

static bool number_ok(const char *s)
{
    if (!s) {
        return false;
    }
    if (*s == '+') {
        s++;
    }
    size_t n = 0;
    for (; *s; s++, n++) {
        if (*s < '0' || *s > '9') {
            return false;
        }
    }
    return n >= 3 && n <= 20;
}

bool sim_tools_request(sim_action_t action, const char *a, const char *b, char *err, size_t err_size)
{
    if (action == SIM_ACT_USSD && !ussd_ok(a)) {
        snprintf(err, err_size, "Codice USSD non valido: ammessi solo cifre e i simboli * # +");
        return false;
    }
    if ((action == SIM_ACT_SMS || action == SIM_ACT_FIND_NUMBER) && !number_ok(a)) {
        snprintf(err, err_size, "Numero non valido: solo cifre, con il + iniziale facoltativo (es. +393511234567)");
        return false;
    }
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
                char *p = malloc(SMS_BUF_SIZE);
                if (p) {
                    p[0] = '\0';
                }
                lock();
                s_sms = p;
                unlock();
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
        case SIM_ACT_FIND_NUMBER:
            // Testo senza accenti (alfabeto GSM) e sotto i 160 caratteri.
            snprintf(s_resp, sizeof(s_resp), "EVONETRTK %s: SMS di prova dalla SIM dati. Il mittente "
                     "di questo SMS e' il numero della SIM: scrivilo nel pannello, scheda SIM.", s_cfg->device_serial);
            set_msg(cellular_link_send_sms(s_arg_a, s_resp)
                        ? "SMS di prova inviato a %s: il mittente che vedi sul telefono e' il numero della SIM"
                        : "Invio dell'SMS di prova a %s fallito (SIM, credito o segnale)", s_arg_a);
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
    // Puntatore tolto sotto lock, memoria liberata fuori: prima il free()
    // avveniva senza lock mentre il pannello poteva copiare il testo in
    // sim_tools_get_sms() (lettura di memoria gia' liberata).
    if (s_sms && now_us - s_sms_at_us > SMS_KEEP_US) {
        lock();
        char *old = s_sms;
        s_sms = NULL;
        unlock();
        free(old);
    }

    read_ids();

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
        len = sizeof(s_st.iccid);
        nvs_get_str(h, "iccid", s_st.iccid, &len);
        len = sizeof(s_st.imei);
        nvs_get_str(h, "imei", s_st.imei, &len);
        uint32_t ids_at = 0;
        nvs_get_u32(h, "ids_at", &ids_at);
        s_st.ids_at = (time_t) ids_at;
        nvs_close(h);
    }
}
