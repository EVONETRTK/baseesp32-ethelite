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
static bool s_inited = false;

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
// ATdebug.ino) per T-ETH-Elite + modulo SIM7600X: PWRKEY basso -> 100ms
// -> alto -> 500ms (durata impulso specifica per SIM7600) -> basso, poi
// fino a 15s di attesa perche' il SIM7600 e' lento ad avviarsi. DTR va
// tenuto basso per evitare che il modem entri in sleep.
//
// Il SIM868 (progetto gemello baseesp32/T-Internet-COM) usa una polarita'
// e una durata diverse - PWRKEY alto per 300ms poi basso, confermato dal
// repository ufficiale LilyGO per quel modulo - e si avvia molto piu' in
// fretta. Lo slot LTE della T-ETH-Elite espone comunque lo stesso pin
// fisico per PWRKEY qualunque sia lo shield innestato, cambia solo la
// sequenza da mandarci.
static void modem_power_on(bool is_sim868)
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

bool cellular_link_init(void)
{
    if (s_inited) {
        return true;
    }

    s_events = xEventGroupCreate();

    app_settings_t settings = settings_get();
    modem_power_on(settings.cellular_is_sim868);

    esp_modem_dte_config_t dte_config = ESP_MODEM_DTE_DEFAULT_CONFIG();
    dte_config.uart_config.port_num = CONFIG_BASEESP32_CELLULAR_UART_NUM;
    dte_config.uart_config.tx_io_num = CONFIG_BASEESP32_CELLULAR_UART_TX_PIN;
    dte_config.uart_config.rx_io_num = CONFIG_BASEESP32_CELLULAR_UART_RX_PIN;
    dte_config.uart_config.rts_io_num = -1;
    dte_config.uart_config.cts_io_num = -1;
    dte_config.uart_config.flow_control = ESP_MODEM_FLOW_CONTROL_NONE;
    dte_config.uart_config.baud_rate = CONFIG_BASEESP32_CELLULAR_UART_BAUD;

    esp_modem_dce_config_t dce_config = ESP_MODEM_DCE_DEFAULT_CONFIG(settings.cellular_apn);

    esp_netif_config_t netif_ppp_config = ESP_NETIF_DEFAULT_PPP();
    s_ppp_netif = esp_netif_new(&netif_ppp_config);
    if (!s_ppp_netif) {
        ESP_LOGE(TAG, "Creazione netif PPP fallita");
        return false;
    }

    // Profilo esp_modem dedicato secondo il modulo fisico montato nello
    // slot LTE (vedi settings.cellular_is_sim868 e modem_power_on() sopra
    // per il perche'). Il SIM7600X e' l'hardware scelto per questo
    // progetto ma mai testato su hardware reale finche' non si acquista il
    // modulo; il SIM868 e' quello gia' disponibile (proveniente dal
    // progetto gemello, li' risultato difettoso) usato per un primo test
    // sullo slot di questa scheda diversa - vedi README.
    esp_modem_dce_device_t dce_device = settings.cellular_is_sim868 ? ESP_MODEM_DCE_SIM800 : ESP_MODEM_DCE_SIM7600;
    s_dce = esp_modem_new_dev(dce_device, &dte_config, &dce_config, s_ppp_netif);
    if (!s_dce) {
        ESP_LOGE(TAG, "Inizializzazione modem %s fallita", settings.cellular_is_sim868 ? "SIM868" : "SIM7600");
        return false;
    }

    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_GOT_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_PPP_LOST_IP, &on_ip_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(NETIF_PPP_STATUS, ESP_EVENT_ANY_ID, &on_ppp_event, NULL));

    s_inited = true;
    return true;
}

bool cellular_link_connect(void)
{
    if (!s_inited || !s_dce) {
        return false;
    }

    xEventGroupClearBits(s_events, CELLULAR_CONNECTED_BIT);

    int rssi_csq = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi_csq, &ber) == ESP_OK) {
        ESP_LOGI(TAG, "Segnale: AT+CSQ rssi=%d (99=sconosciuto/nessuna copertura) ber=%d", rssi_csq, ber);
    } else {
        ESP_LOGW(TAG, "Lettura AT+CSQ fallita (modem non risponde?)");
    }

    // AT+CREG? dice se la SIM e' davvero registrata sulla rete (a
    // differenza di AT+CSQ, che misura solo la potenza del segnale
    // ricevuto da una qualunque cella, indipendentemente dalla SIM).
    char creg_resp[64] = {0};
    if (esp_modem_at(s_dce, "AT+CREG?", creg_resp, 2000) == ESP_OK) {
        ESP_LOGI(TAG, "Registrazione rete: %s", creg_resp);
    }

    // AT+CGATT? dice se la SIM e' davvero agganciata alla rete DATI
    // (packet-switched), a differenza di CREG che vale anche per la sola
    // rete voce/SMS - una SIM puo' essere registrata (CREG) ma non avere
    // un piano/agganciarsi ai dati (CGATT: 0), spiegazione comune per un
    // dial PPP che va sempre in timeout nonostante segnale e registrazione
    // regolari.
    char cgatt_resp[32] = {0};
    if (esp_modem_at(s_dce, "AT+CGATT?", cgatt_resp, 5000) == ESP_OK) {
        ESP_LOGI(TAG, "Aggancio rete dati (CGATT): %s", cgatt_resp);
    } else {
        ESP_LOGW(TAG, "Lettura AT+CGATT? fallita");
    }

    if (esp_modem_set_mode(s_dce, ESP_MODEM_MODE_DATA) != ESP_OK) {
        ESP_LOGE(TAG, "Impossibile entrare in modalita' dati (PPP) - verificare SIM/APN/segnale");
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, CELLULAR_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(CELLULAR_DIAL_TIMEOUT_MS));
    if (!(bits & CELLULAR_CONNECTED_BIT)) {
        ESP_LOGE(TAG, "Timeout attesa IP");
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
        return false;
    }

    return true;
}

void cellular_link_disconnect(void)
{
    if (s_dce) {
        esp_modem_set_mode(s_dce, ESP_MODEM_MODE_COMMAND);
    }
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
    int rssi_csq = 0, ber = 0;
    if (esp_modem_get_signal_quality(s_dce, &rssi_csq, &ber) != ESP_OK) {
        return false;
    }
    if (rssi_csq == 99) { // 99 = non rilevabile/sconosciuto (spec AT+CSQ)
        return false;
    }
    *rssi_dbm = -113 + 2 * rssi_csq;
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
    if (!s_inited || !s_dce) {
        return false;
    }

    // Risposta attesa: +COPS: <mode>,<format>,"<operatore>",<AcT>
    char resp[96] = {0};
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

#else // !CONFIG_BASEESP32_CELLULAR_ENABLE

bool cellular_link_init(void) { return false; }
bool cellular_link_connect(void) { return false; }
void cellular_link_disconnect(void) { }
bool cellular_link_is_connected(void) { return false; }
bool cellular_link_get_signal(int *rssi_dbm) { return false; }
bool cellular_link_get_operator_info(char *operator_out, size_t operator_out_size,
                                      char *tech_out, size_t tech_out_size) { return false; }

#endif
