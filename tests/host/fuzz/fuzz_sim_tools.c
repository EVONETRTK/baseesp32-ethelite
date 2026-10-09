// Fuzzing di main/sim_tools.c: lettura del credito dalle risposte USSD e
// dagli SMS (anche in UCS2 esadecimale), lettura degli SMS, ICCID/IMEI/
// numero dal modem. Le risposte del modem vengono dall'input.
// Input: [opzioni][testo 1] 0x00 [testo 2] 0x00 [testo 3] ...
//   testo 1: risposta a AT+CUSD (credito / codice USSD)
//   testo 2: risposta a AT+CMGL (SMS letti / risposta all'SMS del credito)
//   testo 3, 4, 5: ICCID, IMEI, numero (AT+CNUM)
//   testo 6: argomento della richiesta dal pannello (codice o numero)
// Opzioni: bit 0-2 azione dal pannello (SIM_ACT_*), bit 3 credito via SMS,
// bit 4 modem presente, bit 5 risposte UCS2 (esadecimale).
#include "motore.h"
#include "aiuti.h"

#include <time.h>
#define localtime_r(t, tm) (localtime_s((tm), (t)) == 0 ? (tm) : NULL)
#include "../../../main/sim_tools.c"

// --- testi dall'input e sostituti del modem -----------------------------------------
#define TESTI 6
static const char *g_testo[TESTI];
static size_t g_testo_n[TESTI];
static uint8_t g_opz;

// Come il driver vero: copia al massimo out_size - 1 caratteri.
static bool copia_testo(int i, char *out, size_t out_size)
{
    if (!out_size) return false;
    size_t n = g_testo_n[i] < out_size - 1 ? g_testo_n[i] : out_size - 1;
    memcpy(out, g_testo[i], n);
    out[n] = '\0';
    return g_testo_n[i] > 0;
}

bool cellular_link_modem_present(void) { return (g_opz & 16) != 0; }
bool cellular_link_ussd(const char *code, char *out, size_t out_size) { (void) code; return copia_testo(0, out, out_size); }
bool cellular_link_read_sms(char *out, size_t out_size) { return copia_testo(1, out, out_size); }
bool cellular_link_get_iccid(char *out, size_t out_size) { return copia_testo(2, out, out_size); }
bool cellular_link_get_imei(char *out, size_t out_size) { return copia_testo(3, out, out_size); }
bool cellular_link_get_number(char *out, size_t out_size) { return copia_testo(4, out, out_size); }
bool cellular_link_send_sms(const char *number, const char *text) { (void) number; (void) text; return true; }
bool cellular_link_delete_sms(void) { return true; }
bool cellular_link_write_number(const char *number) { (void) number; return true; }
void alerts_wake(void) {}
bool alerts_send_now(const app_settings_t *s, const char *subject, const char *body)
{
    (void) s; (void) subject;
    FUZZ_VERIFICA(strlen(body) < 256);
    return true;
}
void host_time_advance_us(int64_t us);

static app_settings_t g_cfg;

void bersaglio_avvio(void)
{
    fuzz_imposta_max_len(1500);
    strlcpy(g_cfg.device_serial, "PROVA-0000", sizeof(g_cfg.device_serial));
    strlcpy(g_cfg.sim_credit_code, "*123#", sizeof(g_cfg.sim_credit_code));
    strlcpy(g_cfg.sim_credit_sms_number, "40916", sizeof(g_cfg.sim_credit_sms_number));
    strlcpy(g_cfg.sim_credit_sms_text, "CREDITO", sizeof(g_cfg.sim_credit_sms_text));
    strlcpy(g_cfg.sim_phone, "+39 300 0000000", sizeof(g_cfg.sim_phone));
    g_cfg.sim_credit_min_eur = 5.0f;
}

static void seme(uint8_t opz, const char *const *t, int n)
{
    uint8_t b[1500];
    size_t k = 0;
    b[k++] = opz;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(t[i]);
        memcpy(b + k, t[i], l);
        k += l;
        b[k++] = 0;
    }
    fuzz_seme(b, k);
}

void bersaglio_semi(void)
{
    const char *a[] = { "+CUSD: 0,\"Il tuo credito residuo e' di 12,34 euro. Offerta attiva fino al 31/12/2026\",15",
                        "+CMGL: 1,\"REC READ\",\"40916\",,\"26/10/09,10:00:00+08\"\r\nCredito residuo: 7.50 EUR\r\n\r\nOK",
                        "89390100001234567890F", "860000000000000", "+393000000000", "*123#" };
    seme(0x11, a, 6);
    seme(0x19, a, 6);
    seme(0x14, a, 6);
    const char *b[] = { "+CUSD: 0,\"004300720065006400690074006F00200031002C0035003000200080\",72",
                        "+CMGL: 2,\"REC UNREAD\",\"TIM\",,\"26/10/09\"\r\n00530061006C0064006F003A0020003300200065007500720020AC\r\n",
                        "8939", "86", "", "+393001234567" };
    seme(0x31, b, 6);
    seme(0x36, b, 6);
    const char *c[] = { "Saldo 3,00 euro; 10 GB e 200 min fino al 10/11 ore 23:59", "+CMGL: 3\nx", "", "", "", "#100#" };
    seme(0x12, c, 6);
    static const char *const parole[] = { "+CUSD: 0,\"", "\",15", "+CMGL: ", "\r\n", "euro", "eur", "\xe2\x82\xac", "credito",
                                          "saldo", "residuo", "GB", "min", "sms", "giorni", "ore", ",", ".", "0041", "20AC" };
    for (size_t i = 0; i < sizeof(parole) / sizeof(parole[0]); i++) fuzz_parola(parole[i]);
}

void bersaglio_esegui(const uint8_t *d, size_t n)
{
    if (!n) return;
    g_opz = d[0];
    // testi separati da 0x00 (quelli che mancano sono vuoti)
    const char *p = (const char *) d + 1;
    size_t resta = n - 1;
    for (int i = 0; i < TESTI; i++) {
        size_t l = 0;
        while (l < resta && p[l]) l++;
        g_testo[i] = p;
        g_testo_n[i] = l;
        size_t salta = l < resta ? l + 1 : l;
        p += salta;
        resta -= salta;
    }

    // 1) funzioni di lettura, con il testo contro la pagina protetta
    float eur;
    char *s = fuzz_stringa(g_testo[0], g_testo_n[0]);
    (void) sim_tools_parse_euro(s, &eur);
    s = fuzz_stringa(g_testo[0], g_testo_n[0]);
    if (extract_ussd_text(s)) (void) sim_tools_parse_euro(s, &eur);
    s = fuzz_stringa(g_testo[1], g_testo_n[1]);
    if (extract_last_sms_text(s)) (void) sim_tools_parse_euro(s, &eur);
    s = fuzz_stringa(g_testo[1], g_testo_n[1]);
    decode_ucs2_hex_inplace(s);
    char *o = (char *) fuzz_buffer(24);
    iccid_norm(fuzz_stringa(g_testo[2], g_testo_n[2]), o, 24);
    fuzz_zone_azzera();
    char num[24];
    number_compact(fuzz_stringa(g_testo[4], g_testo_n[4]), num, sizeof(num));
    (void) same_number(num, fuzz_stringa(g_testo[3], g_testo_n[3]));
    fuzz_zone_azzera();

    // 2) il giro vero: richiesta dal pannello e passo del task degli avvisi
    memset(&s_st, 0, sizeof(s_st));
    s_pending = SIM_ACT_NONE;
    s_sms_reply_due_us = 0;
    g_cfg.sim_credit_mode = (g_opz & 8) ? 1 : 0;
    char err[128];
    char *arg = fuzz_stringa(g_testo[5], g_testo_n[5]);
    sim_action_t act = (sim_action_t) (g_opz & 7);
    if (act != SIM_ACT_NONE && act <= SIM_ACT_FIND_NUMBER) {
        (void) sim_tools_request(act, arg, "testo di prova", err, sizeof(err));
    }
    sim_tools_tick(&g_cfg);
    if (s_sms_reply_due_us) { // risposta all'SMS del credito, un minuto dopo
        host_time_advance_us(61LL * 1000000);
        sim_tools_tick(&g_cfg);
    }
    sim_tools_status_t st;
    sim_tools_get_status(&st);
    FUZZ_VERIFICA(strlen(st.credit_text) < sizeof(st.credit_text));
    FUZZ_VERIFICA(strlen(st.last_msg) < sizeof(st.last_msg));
    FUZZ_VERIFICA(strlen(st.iccid) < sizeof(st.iccid) && strlen(st.imei) < sizeof(st.imei));
    size_t cap = 1 + fuzz_caso() % 300;
    char *sms = (char *) fuzz_buffer(cap);
    sim_tools_get_sms(sms, cap); // il pannello copia gli SMS letti
}
