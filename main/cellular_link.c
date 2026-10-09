#include "cellular_link.h"
#include "settings.h"

#include <string.h>
#include <stdlib.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"

static const char *TAG = "cellular_link";

#if CONFIG_BASEESP32_CELLULAR_ENABLE

#include "esp_modem_api.h"
#include "esp_netif_ppp.h"
#include "driver/gpio.h"

#define CELLULAR_CONNECTED_BIT BIT0
#define CELLULAR_DIAL_TIMEOUT_MS 30000

static EventGroupHandle_t s_events;
static esp_modem_dce_t *s_dce = NULL;
static esp_netif_t *s_ppp_netif = NULL;
static volatile bool s_connected = false;
static volatile bool s_inited = false; // impostato dal task di avvio del modem (net_manager.c)
static bool s_is_sim868; // dal momento dell'avvio del modem (niente copie delle impostazioni sullo stack)
static bool s_simcom_std; // idem: modulo SIMCom originale, PWRKEY non collegato
static char s_apn[64];    // idem: APN (esp_modem lo copia, ma resta valido comunque)
static bool s_powered;    // impulso di accensione gia' dato: un secondo impulso spegnerebbe il modem
static volatile bool s_init_tried; // primo avvio del modem concluso (riuscito o no)

// Collegamento dati attivo SENZA CMUX (SIM868, o SIM7600 che rifiuta il
// CMUX): la seriale porta solo il PPP. Un comando AT mandato in questo stato
// finisce dentro il flusso PPP (frame rovinati) e la risposta non arriva mai:
// si aspettava ogni volta il timeout (OLED fermo ~3,5 s a schermata, SIM
// "non presente"). Con questo flag segnale e operatore vengono dalla cache
// letta prima di entrare in modalita' dati, e i comandi SIM falliscono subito.
static volatile bool s_data_no_cmux;
static volatile bool s_sig_cache_ok;
static volatile int s_sig_cache_dbm;
static char s_op_cache[32];
static char s_tech_cache[16];
static volatile bool s_op_cache_ok;

// esp_modem_at() copia la risposta con strlcpy(..., CONFIG_ESP_MODEM_C_API_STR_MAX)
// qualunque sia la dimensione del buffer passato: i buffer da 32/64 byte di
// prima potevano traboccare quando nella risposta capitava un messaggio
// spontaneo lungo del modem (es. una risposta USSD arrivata in ritardo, o
// "*PSUTTZ:..." del SIM868), corrompendo lo stack del chiamante.
#ifdef CONFIG_ESP_MODEM_C_API_STR_MAX
#define AT_RESP_MAX CONFIG_ESP_MODEM_C_API_STR_MAX
#else
#define AT_RESP_MAX 128
#endif

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_PPP_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Connessione dati cellulare attiva, IP: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, CELLULAR_CONNECTED_BIT);
    } else if (event_id == IP_EVENT_PPP_LOST_IP) {
        ESP_LOGW(TAG, "Connessione cellulare persa");
        s_connected = false;
        xEventGroupClearBits(s_events, CELLULAR_CONNECTED_BIT);
    }
}

static void on_ppp_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    ESP_LOGD(TAG, "Evento stato PPP: %ld", (long) event_id);
}

// Sequenza di accensione e pin confermati dal repository ufficiale
// Xinyuan-LilyGO/LilyGo-Modem-Series (examples/ATdebug, utilities.h e
// ATdebug.ino) per T-ETH-Elite + modulo SIM7600 (famiglia SIMCom intera:
// pinout, sequenza PWRKEY e comandi AT sono identici per tutte le varianti
// regionali A/E/G/CE/SA e le sottovarianti Cat4 "-H", verificato sul
// documento ufficiale "SIM7600 Series Hardware Design" - la sigla esatta
// scelta all'acquisto conta solo per le bande radio supportate, non per il
// firmware; per l'Italia serve una variante con banda B20, es. G-H o
// CE-H): PWRKEY basso -> 100ms -> alto -> 500ms (durata impulso specifica
// per SIM7600) -> basso, poi fino a 15s di attesa perche' il SIM7600 e'
// lento ad avviarsi. DTR va tenuto basso per evitare che il modem entri in
// sleep.
//
// Il SIM868 (progetto gemello baseesp32/T-Internet-COM) usa una polarita'
// e una durata diverse - PWRKEY alto per 300ms poi basso, confermato dal
// repository ufficiale LilyGO per quel modulo - e si avvia molto piu' in
// fretta. Lo slot LTE della T-ETH-Elite espone comunque lo stesso pin
// fisico per PWRKEY qualunque sia lo shield innestato, cambia solo la
// sequenza da mandarci.
static void modem_power_on(bool is_sim868, bool simcom_std)
{
#if CONFIG_BASEESP32_CELLULAR_DTR_PIN >= 0
    gpio_config_t dtr_conf = {
        .pin_bit_mask = 1ULL << CONFIG_BASEESP32_CELLULAR_DTR_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&dtr_conf);
    gpio_set_level(CONFIG_BASEESP32_CELLULAR_DTR_PIN, 0);
#endif

#if CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN >= 0
    if (!is_sim868 && simcom_std) {
        // Modulo SIMCom originale: accensione automatica. Il pin resta in
        // alta impedenza, cosi' anche con i DIP POWERKEY/RESET dello shield
        // accesi non tiene il modem in reset (PERST# ha il suo pull-up).
        gpio_reset_pin(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN);
        gpio_set_direction(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, GPIO_MODE_INPUT);
        gpio_set_pull_mode(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, GPIO_FLOATING);
        ESP_LOGI(TAG, "Modulo SIM7600 SIMCom originale: si accende da solo, pin PWRKEY non usato. "
                      "Attesa avvio (fino a 15s)...");
        vTaskDelay(pdMS_TO_TICKS(15000));
        return;
    }
    gpio_config_t io_conf = {
        .pin_bit_mask = 1ULL << CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN,
        .mode = GPIO_MODE_OUTPUT,
    };
    gpio_config(&io_conf);

    if (is_sim868) {
        gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(300));
        gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 0);

        ESP_LOGI(TAG, "Attesa avvio modem SIM868 (fino a 3s)...");
        vTaskDelay(pdMS_TO_TICKS(3000));
    } else {
        gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 0);
        vTaskDelay(pdMS_TO_TICKS(100));
        gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 1);
        vTaskDelay(pdMS_TO_TICKS(500));
        gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 0);

        ESP_LOGI(TAG, "Attesa avvio modem SIM7600 (fino a 15s)...");
        vTaskDelay(pdMS_TO_TICKS(15000));
    }
#endif
}

// Solo i campi del modem, senza copiare tutta la configurazione (~2,4 KB)
// sullo stack (cellular_link_init puo' girare anche nel task della rete).
static void peek_modem_cfg(const app_settings_t *s, void *ctx)
{
    s_is_sim868 = s->cellular_is_sim868;
    s_simcom_std = s->cellular_simcom_std;
    strlcpy(s_apn, s->cellular_apn, sizeof(s_apn));
}

bool cellular_link_init(void)
{
    if (s_inited) {
        return true;
    }

    if (!s_events) {
        s_events = xEventGroupCreate();
    }

    settings_peek(peek_modem_cfg, NULL);
    // Accensione una volta sola: ai tentativi successivi (vedi
    // cellular_link_connect) il modem e' gia' acceso e un altro impulso su
    // PWRKEY lo spegnerebbe.
    if (!s_powered) {
        modem_power_on(s_is_sim868, s_simcom_std);
        s_powered = true;
    }

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.port_num = CONFIG_BASEESP32_CELLULAR_UART_NUM;
    dte_config.uart_config.tx_io_num = CONFIG_BASEESP32_CELLULAR_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_BASEESP32_CELLULAR_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = -1;
    dte_config.uart_config.cts_io_num = -1;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.uart_config.baud_rate = CONFIG_BASEESP32_CELLULAR_UART_BAUD;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(s_apn);

    // Riusato ai tentativi successivi: prima un fallimento qui sotto lasciava
    // il netif allocato e il modem restava inutilizzabile fino al riavvio.
    if (!s_ppp_netif) {
        esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
        s_ppp_netif = esp_netif_new(&netif_ppp_config);
    }
    if (!s_ppp_netif) {
        ESP_LOGE(TAG, "Creazione netif PPP fallita");
        s_init_tried = true;
        return false;
    }

    // Profilo esp_modem dedicato secondo il modulo fisico montato nello
    // slot LTE (vedi settings.cellular_is_sim868 e modem_power_on() sopra
    // per il perche'). SIM7600 (famiglia SIMCom) e' l'hardware scelto per
    // questo progetto ma mai testato su hardware reale finche' non si
    // acquista il modulo; il profilo ESP_MODEM_DCE_SIM7600 di esp_modem
    // copre l'intera famiglia (stesso set di comandi AT per tutte le
    // varianti regionali A/E/G/CE/SA), quindi la sigla esatta scelta
    // all'acquisto (in base alle bande radio necessarie, per l'Italia
    // serve B20 - es. G-H o CE-H) non richiede un profilo diverso. Il
    // SIM868 e' quello gia' disponibile (proveniente dal progetto gemello,
    // li' risultato difettoso) usato per un primo test sullo slot di
    // questa scheda diversa - vedi README.
    esp_modem_dce_device_t dce_device = s_is_sim868 ? ESP_MODEM_DCE_SIM800 : ESP_MODEM_DCE_SIM7600;
    s_dce = esp_modem_new_dev(dce_device, &dte_config, &dce_config, s_ppp_netif);
    if (!s_dce) {
        ESP_LOGE(TAG, "Inizializzazione modem %s fallita (nuovo tentativo al prossimo collegamento)",
                 s_is_sim868 ? "SIM868" : "SIM7600");
        if (!s_is_sim868 && s_simcom_std) {
            ESP_LOGW(TAG, "Modulo SIMCom originale: controlla che i DIP POWERKEY (SW2-5) e RESET (SW3) dello shield "
                          "siano spenti e che il modulo sia ben inserito");
        }
        s_init_tried = true;
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_event, NULL));

    s_init_tried = true;
    s_inited = true;
    return true;
}

static bool read_operator(char *operator_out, size_t operator_out_size, char *tech_out, size_t tech_out_size);

bool cellular_link_connect(void)
{
    // Avvio del modem fallito (es. memoria al momento dell'avvio): si
    // ritenta qui, senza un nuovo impulso di accensione. Solo dopo che il
    // task di avvio ha finito il primo tentativo (s_init_tried).
    if (!s_inited && s_init_tried) {
        cellular_link_init();
    }
    if (!s_inited || !s_dce) {
        return false;
    }

    xEventGroupClearBits(s_events, CELLULAR_CONNECTED_BIT);
    s_data_no_cmux = false; // qui il modem e' in modalita' comando

    int rssi_csq = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi_csq, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "Segnale: AT+CSQ rssi=%d (99=sconosciuto/nessuna copertura) ber=%d", rssi_csq, ber);
        s_sig_cache_ok = rssi_csq != 99;
        s_sig_cache_dbm = -113 + 2 * rssi_csq;
    } else {
        ESP_LOGW(TAG, "Lettura AT+CSQ fallita (modem non risponde?)");
        s_sig_cache_ok = false;
    }
    // Operatore letto ora, per mostrarlo anche durante il collegamento dati
    // senza CMUX (vedi s_data_no_cmux).
    s_op_cache_ok = read_operator(s_op_cache, sizeof(s_op_cache), s_tech_cache, sizeof(s_tech_cache));

    // AT+CREG? dice se la SIM e' davvero registrata sulla rete (a
    // differenza di AT+CSQ, che misura solo la potenza del segnale
    // ricevuto da una qualunque cella, indipendentemente dalla SIM).
    char creg_resp[AT_RESP_MAX] = {0};
    if (esp_modem_at(s_dce, "AT+CREG?", creg_resp, 2000) == ESP_OK) {
        ESP_LOGI(TAG, "Registrazione rete: %s", creg_resp);
    }

    // AT+CGATT? dice se la SIM e' davvero agganciata alla rete DATI
    // (packet-switched), a differenza di CREG che vale anche per la sola
    // rete voce/SMS - una SIM puo' essere registrata (CREG) ma non avere
    // un piano/agganciarsi ai dati (CGATT: 0), spiegazione comune per un
    // dial PPP che va sempre in timeout nonostante segnale e registrazione
    // regolari.
    char cgatt_resp[AT_RESP_MAX] = {0};
    if (esp_modem_at(s_dce, "AT+CGATT?", cgatt_resp, 5000) == ESP_OK) {
        ESP_LOGI(TAG, "Aggancio rete dati (CGATT): %s", cgatt_resp);
    } else {
        ESP_LOGW(TAG, "Lettura AT+CGATT? fallita");
    }

    // SIM7600: CMUX (dati e comandi su canali separati), cosi' credito e
    // SMS si possono chiedere anche con la connessione attiva. Se il modem
    // non lo accetta, o con il SIM868, modalita' dati semplice come prima.
    bool cmux = !s_is_sim868 && esp_modem_set_mode(s_dce, ESP_MODEM_MODE_CMUX) == ESP_OK;
    if (cmux) {
        ESP_LOGI(TAG, "Modem in CMUX: dati e comandi insieme");
    }
    if (!cmux && esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "Impossibile entrare in modalita' dati (PPP) - verificare SIM/APN/segnale");
        return false;
    }
    // Da qui, senza CMUX, la seriale porta il PPP: niente AT da altri task
    // (OLED, pannello, SIM) gia' durante l'attesa dell'indirizzo.
    s_data_no_cmux = !cmux;

    EventBits_t bits = xEventGroupWaitBits(s_events, CELLULAR_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(CELLULAR_DIAL_TIMEOUT_MS));
    if (!(bits & CELLULAR_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Timeout attesa IP");
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        s_data_no_cmux = false;
        return false;
    }

    if (!cmux) {
        ESP_LOGI(TAG, "Collegamento dati senza CMUX: niente comandi AT finche' resta attivo "
                      "(segnale e operatore mostrati come letti prima del collegamento)");
    }
    return true;
}

void cellular_link_disconnect(void)
{
    if (s_dce) {
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }
    s_data_no_cmux = false;
    s_connected = false;
}

bool cellular_link_is_connected(void)
{
    return s_connected;
}

bool cellular_link_get_signal(int *rssi_dbm)
{
    if (!s_inited || !s_dce) {
        return false;
    }
    if (s_data_no_cmux) {
        // Niente AT durante il PPP senza CMUX: valore letto prima del collegamento.
        *rssi_dbm = s_sig_cache_dbm;
        return s_sig_cache_ok;
    }
    int rssi_csq = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi_csq, &ber) != ESP_OK) {
        return false;
    }
    if (rssi_csq == 99) { // 99 = non rilevabile/sconosciuto (spec AT+CSQ)
        return false;
    }
    *rssi_dbm = -113 + 2 * rssi_csq;
    s_sig_cache_dbm = *rssi_dbm;
    s_sig_cache_ok = true;
    return true;
}

// Mappa il codice AcT (access technology) standard 3GPP restituito da
// AT+COPS? su una descrizione 2G/3G/4G/5G leggibile.
static const char *act_to_tech_str(int act)
{
    switch (act) {
    case 0: case 1: case 8:        return "2G (GSM)";
    case 3:                        return "2G (EDGE)";
    case 2: case 4: case 5: case 6: return "3G";
    case 7: case 9: case 13:       return "4G (LTE)";
    case 10: case 11: case 12:     return "5G";
    default:                       return "sconosciuta";
    }
}

bool cellular_link_get_operator_info(char *operator_out, size_t operator_out_size,
                                      char *tech_out, size_t tech_out_size)
{
    if (!s_inited || !s_dce || operator_out_size == 0 || tech_out_size == 0) {
        return false;
    }
    if (s_data_no_cmux) {
        // Niente AT durante il PPP senza CMUX: valore letto prima del collegamento.
        if (!s_op_cache_ok) {
            return false;
        }
        strlcpy(operator_out, s_op_cache, operator_out_size);
        strlcpy(tech_out, s_tech_cache, tech_out_size);
        return true;
    }
    return read_operator(operator_out, operator_out_size, tech_out, tech_out_size);
}

static bool read_operator(char *operator_out, size_t operator_out_size, char *tech_out, size_t tech_out_size)
{
    // Risposta attesa: +COPS: <mode>,<format>,"<operatore>",<AcT>
    char resp[AT_RESP_MAX] = {0};
    if (esp_modem_at(s_dce, "AT+COPS?", resp, 3000) != ESP_OK) {
        return false;
    }

    char *quote1 = strchr(resp, '"');
    if (!quote1) {
        return false; // non ancora registrata: la risposta non contiene un nome operatore
    }
    char *quote2 = strchr(quote1 + 1, '"');
    if (!quote2) {
        return false;
    }

    size_t name_len = (size_t) (quote2 - quote1 - 1);
    if (name_len >= operator_out_size) {
        name_len = operator_out_size - 1;
    }
    memcpy(operator_out, quote1 + 1, name_len);
    operator_out[name_len] = '\0';

    int act = -1;
    if (*(quote2 + 1) == ',') {
        act = atoi(quote2 + 2);
    }
    strncpy(tech_out, act_to_tech_str(act), tech_out_size - 1);
    tech_out[tech_out_size - 1] = '\0';

    return true;
}


// ---------------------------------------------------------------------------
// Comandi per la gestione della SIM (sim_tools.c): USSD, SMS, ICCID.
// Chiamati SOLO dal task degli avvisi (uno alla volta). Con la connessione
// cellulare attiva il SIM7600 lavora in CMUX (canali separati per dati e
// comandi), quindi questi comandi non interrompono l'invio al caster.
// ---------------------------------------------------------------------------

// esp_modem_command() passa al callback tutto quanto ricevuto finora (con
// CONFIG_ESP_MODEM_USE_INFLATABLE_BUFFER_IF_NEEDED attivo anche oltre i 512
// byte del buffer della seriale e anche in CMUX, dove una risposta lunga
// arriva divisa in piu' frame: senza, il callback vedeva solo l'ultimo
// pezzo e USSD lunghi e AT+CMGL fallivano sempre). Buffer e risultato
// statici (un solo comando alla volta).
static char *s_cmd_out;
static size_t s_cmd_out_size;
static const char *s_cmd_until;   // testo che chiude la risposta
static bool s_cmd_found;

// Come strstr() su dati non terminati da '\0'.
static const char *mem_find(const char *hay, size_t hay_len, const char *needle)
{
    size_t nl = strlen(needle);
    if (nl == 0 || hay_len < nl) {
        return NULL;
    }
    for (size_t i = 0; i + nl <= hay_len; i++) {
        if (hay[i] == needle[0] && memcmp(hay + i, needle, nl) == 0) {
            return hay + i;
        }
    }
    return NULL;
}

static esp_err_t cmd_collect_cb(uint8_t *data, size_t len)
{
    const char *d = (const char *) data;
    // Esito cercato su TUTTA la risposta ricevuta, non sulla copia (che puo'
    // essere troncata): con molti SMS nella SIM la risposta di AT+CMGL
    // supera il buffer e "OK" finale non si trovava mai. ERROR solo come
    // risposta del modem, non dentro il testo di un SMS.
    bool error = mem_find(d, len, "\nERROR") != NULL || mem_find(d, len, "+CME ERROR") != NULL ||
                 mem_find(d, len, "+CMS ERROR") != NULL || (len >= 5 && memcmp(d, "ERROR", 5) == 0);
    const char *u = mem_find(d, len, s_cmd_until);
    bool complete = u != NULL;
    if (u && strcmp(s_cmd_until, "+CUSD:") == 0) {
        // USSD: "+CUSD: 0,"testo",15" - aspetta la virgoletta di chiusura
        // seguita dalla fine riga, non solo l'inizio della risposta.
        const char *end = d + len;
        const char *q1 = memchr(u, '"', (size_t) (end - u));
        const char *q2 = NULL;
        for (const char *p = end - 1; q1 && p > q1; p--) {
            if (*p == '"') {
                q2 = p;
                break;
            }
        }
        complete = q1 && q2 && memchr(q2, '\n', (size_t) (end - q2)) != NULL;
    }

    // Copia per il chiamante: se non ci sta, la parte FINALE (per AT+CMGL
    // gli SMS piu' recenti, che sono quelli che servono).
    size_t n = len < s_cmd_out_size - 1 ? len : s_cmd_out_size - 1;
    memcpy(s_cmd_out, d + (len - n), n);
    s_cmd_out[n] = '\0';

    if (error && !complete) {
        return ESP_FAIL;
    }
    if (complete) {
        s_cmd_found = true;
        return ESP_OK;
    }
    return ESP_ERR_TIMEOUT; // continua ad aspettare
}

static bool modem_cmd(const char *cmd, const char *until, char *out, size_t out_size, uint32_t timeout_ms)
{
    if (!s_inited || !s_dce || out_size < 2) {
        return false;
    }
    if (s_data_no_cmux) {
        // Collegamento dati senza CMUX: il comando finirebbe nel flusso PPP.
        out[0] = '\0';
        return false;
    }
    s_cmd_out = out;
    s_cmd_out_size = out_size;
    s_cmd_until = until;
    s_cmd_found = false;
    out[0] = '\0';
    esp_err_t err = esp_modem_command(s_dce, cmd, cmd_collect_cb, timeout_ms);
    return err == ESP_OK && s_cmd_found;
}

bool cellular_link_reset_modem(void)
{
    if (!s_inited || !s_dce) {
        return false;
    }
    xEventGroupClearBits(s_events, CELLULAR_CONNECTED_BIT);
    s_connected = false;
    esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    s_data_no_cmux = false;
    char resp[AT_RESP_MAX] = {0};
    if (esp_modem_at(s_dce, "AT+CFUN=1,1", resp, 5000) == ESP_OK) {
        ESP_LOGW(TAG, "Modem riavviato con AT+CFUN=1,1, attesa avvio (20 s)");
        vTaskDelay(pdMS_TO_TICKS(20000));
        return true;
    }
#if CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN >= 0
    if (!s_is_sim868 && s_simcom_std) {
        ESP_LOGE(TAG, "Il modem non risponde e il modulo SIMCom originale non si puo' spegnere dal "
                      "firmware (PWRKEY scollegato): serve un riavvio dell'alimentazione");
        return false;
    }
    // Il modem non risponde: spegnimento con un impulso lungo su PWRKEY
    // (SIM7600 >= 2,5 s, SIM868 >= 1 s; stesso verso dell'impulso di
    // accensione), poi riaccensione normale.
    ESP_LOGW(TAG, "Il modem non risponde ai comandi: spegnimento e riaccensione con PWRKEY");
    gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 1);
    vTaskDelay(pdMS_TO_TICKS(s_is_sim868 ? 1500 : 3000));
    gpio_set_level(CONFIG_BASEESP32_CELLULAR_PWRKEY_PIN, 0);
    vTaskDelay(pdMS_TO_TICKS(10000));
    modem_power_on(s_is_sim868, s_simcom_std);
    return true;
#else
    ESP_LOGE(TAG, "Il modem non risponde e il pin PWRKEY non e' configurato");
    return false;
#endif
}

bool cellular_link_modem_present(void)
{
    if (!s_inited || !s_dce) {
        return false;
    }
    if (s_data_no_cmux) {
        return true; // collegamento dati attivo: il modem c'e', ma niente AT ora
    }
    char resp[AT_RESP_MAX];
    return esp_modem_at(s_dce, "AT", resp, 1000) == ESP_OK;
}

// Difesa anche qui, oltre ai controlli del pannello: codici e numeri
// finiscono dentro un comando AT tra virgolette, e un '"' o un a capo
// permetterebbero di accodare comandi arbitrari al modem.
// Codice USSD: solo cifre, '*', '#' e '+'.
static bool ussd_code_ok(const char *code)
{
    size_t n = 0;
    for (const char *p = code; *p; p++, n++) {
        if (!((*p >= '0' && *p <= '9') || *p == '*' || *p == '#' || *p == '+')) {
            return false;
        }
    }
    return n > 0 && n <= 40;
}

// Numero di telefono: '+' facoltativo seguito solo da cifre (3-20). Senza
// '+' servono i numeri brevi degli operatori (es. quello per il credito).
static bool phone_number_ok(const char *number)
{
    const char *p = number[0] == '+' ? number + 1 : number;
    size_t n = 0;
    for (; *p; p++, n++) {
        if (*p < '0' || *p > '9') {
            return false;
        }
    }
    return n >= 3 && n <= 20;
}

// Testo SMS: niente caratteri di controllo (Ctrl-Z chiude il messaggio,
// ESC lo annulla: con questi il resto del testo andrebbe al modem come comando).
static bool sms_text_ok(const char *text)
{
    for (const unsigned char *p = (const unsigned char *) text; *p; p++) {
        if (*p < 0x20 || *p == 0x7F) {
            return false;
        }
    }
    return true;
}

bool cellular_link_get_iccid(char *out, size_t out_size)
{
    char buf[96];
    if (!modem_cmd("AT+CICCID\r", "OK", buf, sizeof(buf), 3000)) {
        return false;
    }
    const char *p = strstr(buf, "ICCID:");
    p = p ? p + 6 : buf;
    while (*p == ' ' || *p == '\r' || *p == '\n') p++;
    size_t n = 0;
    while (p[n] && p[n] != '\r' && p[n] != '\n' && n < out_size - 1) n++;
    memcpy(out, p, n);
    out[n] = '\0';
    return n > 0;
}

bool cellular_link_get_number(char *out, size_t out_size)
{
    // Risposta: +CNUM: "","+393511234567",145 - il numero e' la prima
    // stringa tra virgolette che inizia con + o una cifra (la prima,
    // l'etichetta, di solito e' vuota). Solo "OK" = numero non scritto nella SIM.
    char buf[128];
    out[0] = '\0';
    if (!modem_cmd("AT+CNUM\r", "OK", buf, sizeof(buf), 3000)) {
        return false;
    }
    const char *p = strstr(buf, "+CNUM:");
    if (!p) {
        return true;
    }
    p += 6;
    while ((p = strchr(p, '"')) != NULL) {
        const char *e = strchr(p + 1, '"');
        if (!e) {
            break;
        }
        if (e > p + 1 && (p[1] == '+' || (p[1] >= '0' && p[1] <= '9'))) {
            size_t n = (size_t) (e - p - 1);
            if (n > out_size - 1) {
                n = out_size - 1;
            }
            memcpy(out, p + 1, n);
            out[n] = '\0';
            return true;
        }
        p = e + 1;
    }
    return true;
}

bool cellular_link_get_imei(char *out, size_t out_size)
{
    // Risposta: "AT+CGSN\r\r\n861234567890123\r\n\r\nOK" (con o senza eco):
    // l'IMEI e' la prima sequenza di 14-17 cifre (15 di solito).
    char buf[96];
    out[0] = '\0';
    if (!modem_cmd("AT+CGSN\r", "OK", buf, sizeof(buf), 3000)) {
        return false;
    }
    for (const char *p = buf; *p; ) {
        if (*p < '0' || *p > '9') {
            p++;
            continue;
        }
        size_t n = 0;
        while (p[n] >= '0' && p[n] <= '9') n++;
        if (n >= 14 && n <= 17 && n < out_size) {
            memcpy(out, p, n);
            out[n] = '\0';
            return true;
        }
        p += n;
    }
    return false;
}

bool cellular_link_write_number(const char *number)
{
    // Rubrica "ON" (own numbers) della SIM: la stessa che legge AT+CNUM.
    // Molte SIM la accettano senza PIN2; se la rifiutano, ERROR e false.
    char cmd[80];
    char tmp[48];
    if (!number || !phone_number_ok(number)) {
        ESP_LOGW(TAG, "Numero da scrivere nella SIM non valido: rifiutato");
        return false;
    }
    if (!modem_cmd("AT+CSCS=\"GSM\"\r", "OK", tmp, sizeof(tmp), 2000) ||
        !modem_cmd("AT+CPBS=\"ON\"\r", "OK", tmp, sizeof(tmp), 3000)) {
        return false;
    }
    snprintf(cmd, sizeof(cmd), "AT+CPBW=1,\"%s\",%d,\"Numero\"\r", number, number[0] == '+' ? 145 : 129);
    bool ok = modem_cmd(cmd, "OK", tmp, sizeof(tmp), 5000);
    modem_cmd("AT+CPBS=\"SM\"\r", "OK", tmp, sizeof(tmp), 3000); // rubrica normale, come prima
    return ok;
}

bool cellular_link_ussd(const char *code, char *out, size_t out_size)
{
    char cmd[64];
    // Testo in "GSM" (leggibile); alcuni operatori rispondono comunque in
    // UCS2 esadecimale: lo decodifica sim_tools.c.
    char tmp[32];
    if (!code || !ussd_code_ok(code)) {
        ESP_LOGW(TAG, "Codice USSD non valido (ammessi solo cifre, * # +): rifiutato");
        if (out_size) {
            out[0] = '\0';
        }
        return false;
    }
    modem_cmd("AT+CSCS=\"GSM\"\r", "OK", tmp, sizeof(tmp), 2000);
    snprintf(cmd, sizeof(cmd), "AT+CUSD=1,\"%s\",15\r", code);
    return modem_cmd(cmd, "+CUSD:", out, out_size, 30000);
}

bool cellular_link_send_sms(const char *number, const char *text)
{
    if (!s_inited || !s_dce || s_data_no_cmux) {
        return false;
    }
    if (!number || !text || !phone_number_ok(number) || !sms_text_ok(text)) {
        ESP_LOGW(TAG, "SMS rifiutato: numero non valido ('+' facoltativo e solo cifre) o testo con caratteri di controllo");
        return false;
    }
    esp_modem_sms_txt_mode(s_dce, true);
    esp_modem_sms_character_set(s_dce);
    return esp_modem_send_sms(s_dce, number, text) == ESP_OK;
}

bool cellular_link_read_sms(char *out, size_t out_size)
{
    char tmp[32];
    if (!s_inited || !s_dce || s_data_no_cmux) {
        return false;
    }
    esp_modem_sms_txt_mode(s_dce, true);
    modem_cmd("AT+CSCS=\"GSM\"\r", "OK", tmp, sizeof(tmp), 2000);
    return modem_cmd("AT+CMGL=\"ALL\"\r", "\r\nOK", out, out_size, 15000);
}

bool cellular_link_delete_sms(void)
{
    char tmp[32];
    return modem_cmd("AT+CMGD=1,4\r", "OK", tmp, sizeof(tmp), 10000);
}

#else // !CONFIG_BASEESP32_CELLULAR_ENABLE

bool cellular_link_init(void) { return false; }
bool cellular_link_connect(void) { return false; }
void cellular_link_disconnect(void) { }
bool cellular_link_is_connected(void) { return false; }
bool cellular_link_get_signal(int *rssi_dbm) { return false; }
bool cellular_link_get_operator_info(char *operator_out, size_t operator_out_size,
                                      char *tech_out, size_t tech_out_size) { return false; }
bool cellular_link_modem_present(void) { return false; }
bool cellular_link_reset_modem(void) { return false; }
bool cellular_link_get_iccid(char *out, size_t out_size) { return false; }
bool cellular_link_get_number(char *out, size_t out_size) { return false; }
bool cellular_link_get_imei(char *out, size_t out_size) { return false; }
bool cellular_link_write_number(const char *number) { return false; }
bool cellular_link_ussd(const char *code, char *out, size_t out_size) { return false; }
bool cellular_link_send_sms(const char *number, const char *text) { return false; }
bool cellular_link_read_sms(char *out, size_t out_size) { return false; }
bool cellular_link_delete_sms(void) { return false; }

#endif
