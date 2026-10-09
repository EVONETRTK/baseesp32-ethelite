#include "net_manager.h"
#include "wifi_link.h"
#include "cellular_link.h"
#include "eth_link.h"
#include "status.h"
#include "settings.h"
#include "sys_stats.h"
#include "vpn_link.h"
#include "web_ui.h"

#include "net_util.h"

#include <string.h>
#include <stdio.h>
#include <errno.h>
#include <netdb.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_manager";

// eth_link_init() (bus SPI + installazione driver W5500, diversi livelli di
// chiamata annidati dentro esp_eth) fatta girare direttamente nel task
// "main" (app_main(), stack CONFIG_ESP_MAIN_TASK_STACK_SIZE=8192) causava
// uno stack overflow reale, confermato su hardware (crash + riavvio
// automatico ad ogni singolo boot, sempre nello stesso punto) - stesso
// problema, stessa causa e stessa soluzione gia' visti piu' volte in
// questo progetto per altre operazioni "pesanti" (scansione WiFi,
// archiviazione firmware su SD): isolata in un task dedicato con stack
// suo, invece di continuare ad allargare lo stack del task main ad ogni
// nuova funzionalita' che lo tocca.
static void eth_link_init_task(void *arg)
{
    eth_link_init();
    vTaskDelete(NULL);
}

typedef enum {
    LINK_NONE,
    LINK_WIFI,
    LINK_CELLULAR,
} active_link_t;

static void peek_network_mode(const app_settings_t *s, void *ctx)
{
    *(network_mode_t *) ctx = s->network_mode;
}

// ---------------------------------------------------------------------------
// Controllo che la rete attiva porti davvero a Internet. Prima si lasciava
// il WiFi solo se cadeva l'associazione e la SIM solo se il PPP lo
// segnalava: con il router acceso ma la sua linea Internet guasta la base
// restava sul WiFi (niente passaggio alla SIM in "WiFi + cellulare"), e con
// un PPP morto senza avviso il rover restava "connesso" per sempre senza
// correzioni. Ora: se il caster non e' collegato da 3 minuti, si prova una
// connessione TCP al caster (stesso host e porta) e, se non risponde, a due
// servizi pubblici; 3 prove fallite di fila (una al minuto) = rete senza
// Internet, si cambia o si ricollega.
// Una risposta qualunque del caster (anche un rifiuto) vuol dire che la
// rete funziona: il problema e' altrove (password, mountpoint...).
// ---------------------------------------------------------------------------
#define NET_CHECK_AFTER_US      (3LL * 60 * 1000000)   // caster scollegato da tanto...
#define NET_PROBE_EVERY_US      (60LL * 1000000)       // ...prova ogni minuto...
#define NET_PROBE_OK_PAUSE_US   (5LL * 60 * 1000000)   // ...e dopo una prova riuscita, ogni 5
#define NET_PROBE_FAILS         3
#define WIFI_HOLD_US            (15LL * 60 * 1000000)  // WiFi senza Internet: SIM per almeno 15 minuti
#define ETH_ONLY_AP_AFTER_US    (3LL * 60 * 1000000)   // "Solo Ethernet" senza rete: AP di emergenza

typedef struct {
    char host[64];
    uint16_t port;
} probe_target_t;

static void peek_probe_target(const app_settings_t *s, void *ctx)
{
    probe_target_t *t = (probe_target_t *) ctx;
    strlcpy(t->host, s->ntrip_host, sizeof(t->host));
    t->port = s->ntrip_port;
}

// Connessione TCP a un indirizzo numerico (senza DNS): true se risponde.
static bool tcp_reachable(const char *ip, uint16_t port, int timeout_s)
{
    struct sockaddr_in a = { .sin_family = AF_INET, .sin_port = htons(port) };
    if (inet_pton(AF_INET, ip, &a.sin_addr) != 1) {
        return false;
    }
    int sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock < 0) {
        return true; // socket esauriti: non e' colpa della rete
    }
    int rc = net_connect_timeout(sock, (struct sockaddr *) &a, sizeof(a), timeout_s);
    int e = errno;
    net_close_now(sock);
    return rc == 0 || e == ECONNREFUSED || e == ECONNRESET;
}

static bool caster_reachable(void);

// true = Internet raggiungibile. Prima il caster; se non risponde, due
// servizi pubblici per indirizzo numerico: un caster spento o in
// manutenzione non deve far cambiare rete (con la SIM si pagherebbero i dati
// per niente).
static bool internet_probe(void)
{
    if (caster_reachable()) {
        return true;
    }
    if (tcp_reachable("8.8.8.8", 53, 5) || tcp_reachable("1.1.1.1", 443, 5)) {
        ESP_LOGW(TAG, "Controllo rete: caster irraggiungibile ma Internet funziona (problema del caster, non della rete)");
        return true;
    }
    return false;
}

static bool caster_reachable(void)
{
    static probe_target_t t; // statica: niente struct sullo stack
    settings_peek(peek_probe_target, &t);
    if (t.host[0] == '\0' || t.port == 0) {
        return false; // niente caster impostato: decidono i servizi pubblici
    }
    char port_str[8];
    snprintf(port_str, sizeof(port_str), "%u", t.port);
    struct addrinfo hints = { .ai_family = AF_INET, .ai_socktype = SOCK_STREAM };
    struct addrinfo *res = NULL;
    if (getaddrinfo(t.host, port_str, &hints, &res) != 0 || res == NULL) {
        ESP_LOGW(TAG, "Controllo rete: nome del caster %s non risolto (DNS)", t.host);
        return false;
    }
    int sock = socket(res->ai_family, res->ai_socktype, res->ai_protocol);
    if (sock < 0) {
        freeaddrinfo(res);
        return true; // socket esauriti: non e' colpa della rete
    }
    int rc = net_connect_timeout(sock, res->ai_addr, res->ai_addrlen, 8);
    int e = errno;
    net_close_now(sock);
    freeaddrinfo(res);
    if (rc == 0 || e == ECONNREFUSED || e == ECONNRESET) {
        return true; // il caster (o il suo host) ha risposto
    }
    ESP_LOGW(TAG, "Controllo rete: caster %s:%u non raggiungibile (errno %d)", t.host, t.port, e);
    return false;
}

typedef struct {
    int64_t no_caster_since_us;
    int64_t next_probe_us;
    int fails;
} net_check_t;

static void net_check_reset(net_check_t *c)
{
    c->no_caster_since_us = 0;
    c->next_probe_us = 0;
    c->fails = 0;
}

// true = la rete attiva va considerata senza Internet.
static bool net_check_dead(net_check_t *c)
{
    int64_t now = esp_timer_get_time();
    ntrip_conn_status_t nt = status_ntrip_get();
    if (nt.connected) {
        net_check_reset(c);
        return false;
    }
    if (c->no_caster_since_us == 0) {
        c->no_caster_since_us = now;
        return false;
    }
    if (now - c->no_caster_since_us < NET_CHECK_AFTER_US || now < c->next_probe_us) {
        return false;
    }
    sys_stats_heartbeat(HB_NET); // la prova puo' durare fino a ~20 s (DNS + connessione)
    if (internet_probe()) {
        c->fails = 0;
        c->next_probe_us = now + NET_PROBE_OK_PAUSE_US;
        return false;
    }
    c->next_probe_us = now + NET_PROBE_EVERY_US;
    if (++c->fails < NET_PROBE_FAILS) {
        return false;
    }
    net_check_reset(c);
    return true;
}

static void net_manager_task(void *arg)
{
    active_link_t active = LINK_NONE;
    TickType_t cell_since = 0; // ultimo passaggio al cellulare o tentativo di tornare al WiFi
    // Tentativi cellulari falliti di fila e ultimo riavvio del modem: dopo 3
    // fallimenti il modem si riavvia (al massimo ogni 10 minuti). Un modem
    // LTE "piantato" altrimenti resta tale finche' qualcuno non va sul posto.
    int cell_fail = 0;
    TickType_t modem_reset_at = 0;
    bool modem_reset_done = false;
    static net_check_t check; // controllo di Internet sulla rete attiva
    int64_t wifi_hold_until_us = 0; // WiFi senza Internet: niente WiFi fino a questo istante
    int64_t eth_down_since_us = 0;  // "Solo Ethernet": da quando manca la rete via cavo
    int cell_dead = 0;              // collegamenti cellulari "senza Internet" di fila
    int64_t eth_probe_next_us = 0;  // prossimo controllo di Internet via cavo
    bool eth_probe_ok = false;

    while (1) {
        sys_stats_heartbeat(HB_NET);
        vpn_link_tick(); // tunnel VPN per l'accesso remoto (vpn_link.c)
        // Solo la modalita' di rete, senza copiare tutta la configurazione
        // (~2,2 KB): con la copia, piu' quella dentro wifi_link_connect_known()
        // e l'elenco della scansione WiFi, restavano 416 byte di stack liberi
        // e la crescita della configurazione nella 1.19.92 lo faceva traboccare
        // (crash rari dopo gli aggiornamenti, ottobre 2026).
        struct { network_mode_t network_mode; } settings;
        settings_peek(peek_network_mode, &settings.network_mode);
        int64_t now_us = esp_timer_get_time();

        switch (active) {
        case LINK_NONE:
            if (settings.network_mode == NETWORK_MODE_ETHERNET_ONLY) {
                // Radio WiFi non avviata (vedi net_manager_start): niente
                // da tentare qui, l'unica connettivita' e' l'Ethernet
                // indipendente gestita sotto. Evita anche di chiamare
                // funzioni wifi_link_* su un driver mai inizializzato.
                status_set_net(NET_STATUS_NONE);
                // Cavo scollegato o nessun indirizzo per 3 minuti: si accende
                // comunque la rete di setup (AP), altrimenti la base restava
                // irraggiungibile e solo il tasto di reset la recuperava.
                if (eth_link_is_connected()) {
                    eth_down_since_us = 0;
                } else if (eth_down_since_us == 0) {
                    eth_down_since_us = now_us;
                } else if (!wifi_link_is_started() && now_us - eth_down_since_us >= ETH_ONLY_AP_AFTER_US) {
                    ESP_LOGW(TAG, "Solo Ethernet: nessuna rete via cavo da %lld s, accendo la rete di setup (AP) "
                                  "di emergenza per non perdere l'accesso al pannello",
                             (long long) ((now_us - eth_down_since_us) / 1000000));
                    wifi_link_init();
                }
                vTaskDelay(pdMS_TO_TICKS(CONFIG_BASEESP32_NET_RETRY_DELAY_MS));
                break;
            }
            if (wifi_link_is_connected() && settings.network_mode != NETWORK_MODE_CELLULAR_ONLY) {
                // WiFi gia' collegato (es. "Connetti" dal pannello appena
                // riuscito): lo si adotta, senza scollegarlo e ricollegarlo
                // come faceva wifi_link_connect_known() (caduta di qualche
                // secondo e caster scollegato subito dopo la prova riuscita).
                ESP_LOGI(TAG, "Rete attiva: WiFi (gia' collegato)");
                status_set_net(NET_STATUS_WIFI);
                net_check_reset(&check);
                active = LINK_WIFI;
                break;
            }
            if (web_ui_wifi_test_in_progress()) {
                // Un test manuale ("Connetti" dalla UI) e' in corso: non
                // avviare un tentativo automatico in parallelo, altrimenti
                // i due si contendono la stessa radio WiFi e quello
                // automatico puo' vincere per ultimo, riportando il
                // dispositivo sulla rete vecchia subito dopo che l'utente
                // ha appena verificato con successo quella nuova - vedi
                // web_ui_wifi_test_in_progress() per i dettagli del bug
                // osservato. Si riprova al giro successivo del ciclo.
                vTaskDelay(pdMS_TO_TICKS(2000));
                break;
            }
            // In "WiFi + cellulare", dopo un WiFi senza Internet si passa
            // subito alla SIM per un po' (vedi net_check_dead).
            bool wifi_on_hold = settings.network_mode == NETWORK_MODE_BOTH && now_us < wifi_hold_until_us;
            if (settings.network_mode != NETWORK_MODE_CELLULAR_ONLY && !wifi_on_hold) {
                ESP_LOGI(TAG, "Tentativo connessione WiFi...");
                if (wifi_link_connect_known(CONFIG_BASEESP32_WIFI_CONNECT_TIMEOUT_MS)) {
                    ESP_LOGI(TAG, "Rete attiva: WiFi");
                    status_set_net(NET_STATUS_WIFI);
                    net_check_reset(&check);
                    active = LINK_WIFI;
                    break;
                }
                wifi_link_disconnect();
            }

            // Internet dal cavo Ethernet (con gateway e caster raggiungibile):
            // niente SIM. Serve perche' l'Ethernet ora ha priorita' di
            // instradamento piu' bassa del PPP (vedi eth_link_init): con la
            // SIM accesa il traffico passerebbe dalla SIM, a pagamento.
            bool eth_inet = false;
            if (eth_link_has_gateway()) {
                if (now_us >= eth_probe_next_us) {
                    eth_probe_ok = internet_probe();
                    eth_probe_next_us = esp_timer_get_time() + (eth_probe_ok ? NET_PROBE_OK_PAUSE_US : NET_PROBE_EVERY_US);
                }
                eth_inet = eth_probe_ok;
            }
            if (eth_inet) {
                status_set_net(NET_STATUS_NONE); // WiFi/SIM spenti: si lavora via cavo
                vTaskDelay(pdMS_TO_TICKS(CONFIG_BASEESP32_NET_RETRY_DELAY_MS));
                break;
            }

            if (settings.network_mode != NETWORK_MODE_WIFI_ONLY) {
                ESP_LOGW(TAG, "Tentativo rete cellulare...");
                if (cellular_link_connect()) {
                    cell_fail = 0;
                    ESP_LOGI(TAG, "Rete attiva: cellulare");
                    status_set_net(NET_STATUS_CELLULAR);
                    net_check_reset(&check);
                    active = LINK_CELLULAR;
                    cell_since = xTaskGetTickCount();
                    break;
                }
                // SIM non disponibile: al prossimo giro si riprova anche il
                // WiFi, meglio un WiFi incerto che nessuna rete.
                wifi_hold_until_us = 0;
                if (++cell_fail >= 3 &&
                    (!modem_reset_done || xTaskGetTickCount() - modem_reset_at >= pdMS_TO_TICKS(10 * 60 * 1000))) {
                    ESP_LOGW(TAG, "Rete cellulare: %d tentativi falliti di fila, riavvio del modem", cell_fail);
                    sys_stats_heartbeat(HB_NET); // il riavvio del modem dura fino a ~30 s
                    cellular_link_reset_modem();
                    modem_reset_done = true;
                    modem_reset_at = xTaskGetTickCount();
                    cell_fail = 0;
                }
            }

            status_set_net(NET_STATUS_NONE);
            ESP_LOGE(TAG, "Nessuna rete disponibile (l'AP di setup resta comunque attivo), nuovo tentativo tra %d ms",
                     CONFIG_BASEESP32_NET_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BASEESP32_NET_RETRY_DELAY_MS));
            break;

        case LINK_WIFI:
            if (web_ui_wifi_test_in_progress()) {
                // "Connetti" dal pannello in corso: la STA risulta scollegata
                // per qualche secondo mentre si collega alla nuova rete; prima
                // qui la si scollegava di nuovo, interrompendo la prova.
                vTaskDelay(pdMS_TO_TICKS(2000));
                break;
            }
            if (!wifi_link_is_connected()) {
                ESP_LOGW(TAG, "WiFi perso, ricomincio la selezione rete");
                wifi_link_disconnect();
                status_set_net(NET_STATUS_NONE);
                active = LINK_NONE;
            } else if (net_check_dead(&check)) {
                if (settings.network_mode == NETWORK_MODE_BOTH) {
                    ESP_LOGW(TAG, "WiFi collegato ma senza Internet (caster irraggiungibile): passo alla SIM "
                                  "per almeno %d minuti", (int) (WIFI_HOLD_US / 60000000));
                    wifi_hold_until_us = esp_timer_get_time() + WIFI_HOLD_US;
                } else {
                    ESP_LOGW(TAG, "WiFi collegato ma senza Internet (caster irraggiungibile): ricollego il WiFi");
                }
                wifi_link_disconnect();
                status_set_net(NET_STATUS_NONE);
                active = LINK_NONE;
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            break;

        case LINK_CELLULAR:
            if (cell_dead > 0 && status_ntrip_get().connected) {
                cell_dead = 0; // il caster risponde di nuovo: conteggio azzerato
            }
            if (!cellular_link_is_connected()) {
                ESP_LOGW(TAG, "GPRS perso, ricomincio la selezione rete (si ritenta prima il WiFi)");
                cellular_link_disconnect();
                status_set_net(NET_STATUS_NONE);
                active = LINK_NONE;
            } else if (net_check_dead(&check)) {
                // PPP "su" ma senza traffico (modem rimasto in sessione senza
                // copertura): si richiude e si ricompone. I fallimenti
                // contano per il riavvio del modem come quelli del collegamento.
                ESP_LOGW(TAG, "Cellulare collegato ma senza Internet (caster irraggiungibile): richiudo e ricollego");
                cellular_link_disconnect();
                status_set_net(NET_STATUS_NONE);
                active = LINK_NONE;
                // Due volte di fila senza mai vedere il caster: riavvio del
                // modem (stesso limite di uno ogni 10 minuti).
                if (++cell_dead >= 2 &&
                    (!modem_reset_done || xTaskGetTickCount() - modem_reset_at >= pdMS_TO_TICKS(10 * 60 * 1000))) {
                    ESP_LOGW(TAG, "Cellulare senza Internet per %d volte di fila: riavvio del modem", cell_dead);
                    sys_stats_heartbeat(HB_NET);
                    cellular_link_reset_modem();
                    modem_reset_done = true;
                    modem_reset_at = xTaskGetTickCount();
                    cell_dead = 0;
                }
            } else if (settings.network_mode != NETWORK_MODE_CELLULAR_ONLY && wifi_link_is_connected() &&
                       !web_ui_wifi_test_in_progress()) {
                // WiFi collegato dal pannello ("Connetti") mentre si era sulla
                // SIM: si passa subito al WiFi e si chiude il cellulare (prima
                // la SIM restava accesa fino a 5 minuti, con il traffico
                // contato come cellulare).
                ESP_LOGI(TAG, "WiFi collegato dal pannello: lascio il cellulare");
                cellular_link_disconnect();
                status_set_net(NET_STATUS_WIFI);
                net_check_reset(&check);
                active = LINK_WIFI;
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
                // Ogni 5 minuti si riprova il WiFi: prima, una volta passata al
                // cellulare (WiFi caduto), la base ci restava per sempre anche
                // con il WiFi tornato, consumando i dati della SIM. Il WiFi si
                // collega mentre il cellulare e' ancora attivo; solo se riesce
                // si chiude il cellulare (le connessioni al caster ripartono).
                // Dopo un WiFi senza Internet si aspetta la fine della pausa.
                if (settings.network_mode != NETWORK_MODE_CELLULAR_ONLY &&
                    xTaskGetTickCount() - cell_since >= pdMS_TO_TICKS(5 * 60 * 1000) &&
                    esp_timer_get_time() >= wifi_hold_until_us &&
                    !web_ui_wifi_test_in_progress()) {
                    cell_since = xTaskGetTickCount();
                    if (wifi_link_connect_known(CONFIG_BASEESP32_WIFI_CONNECT_TIMEOUT_MS)) {
                        ESP_LOGI(TAG, "WiFi di nuovo disponibile: lascio il cellulare");
                        cellular_link_disconnect();
                        status_set_net(NET_STATUS_WIFI);
                        net_check_reset(&check);
                        active = LINK_WIFI;
                    } else {
                        wifi_link_disconnect();
                    }
                }
            }
            break;
        }
    }
}

static void cellular_init_task(void *arg)
{
    cellular_link_init();
    vTaskDelete(NULL);
}

void net_manager_start(void)
{
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // Un nome fisso (es. http://EVONETRTK-893428.local) invece del solo
    // IP - utile perche' l'indirizzo cambia a seconda della rete a cui ci
    // si collega (AP di setup isolato vs rete di casa) e puo' comunque
    // variare nel tempo (DHCP). Derivato dall'SSID dell'AP: gia' unico
    // per dispositivo (suffisso dal MAC), cosi' piu' basi/rover sulla
    // stessa rete non si scontrano.
    app_settings_t settings; // riempita senza copie temporanee (vedi settings_get_into)
    settings_get_into(&settings);
    esp_err_t mdns_err = mdns_init();
    if (mdns_err == ESP_OK) {
        mdns_hostname_set(settings.ap_ssid);
        mdns_instance_name_set("EVONETRTK RTK base/rover");
        mdns_service_add(NULL, "_http", "_tcp", 80, NULL, 0);
        ESP_LOGI(TAG, "mDNS attivo: raggiungibile anche su http://%s.local", settings.ap_ssid);
    } else {
        ESP_LOGW(TAG, "Init mDNS fallita: %s", esp_err_to_name(mdns_err));
    }

    // wifi_link_init() porta su anche l'AP di setup, sempre attivo: non
    // blocchiamo l'avvio in attesa di WiFi/GPRS, il resto del firmware
    // (UI web, task GNSS/NTRIP) deve partire comunque.
    // NETWORK_MODE_ETHERNET_ONLY e' l'unica eccezione voluta: qui la radio
    // WiFi non si accende proprio (non solo "non si connette"), per poter
    // isolare interferenze RF sul GNSS - perde la garanzia "AP sempre
    // raggiungibile", il tasto fisico di reset resta l'unico recupero.
    if (settings.network_mode != NETWORK_MODE_ETHERNET_ONLY) {
        wifi_link_init();
    } else {
        ESP_LOGW(TAG, "NETWORK_MODE_ETHERNET_ONLY: radio WiFi non avviata, solo Ethernet");
    }
    // Avvio del modem in un task a parte: con un SIM7600 aspetta fino a 15 s
    // che il modulo si accenda, e chiamato qui bloccava tutto l'avvio (WiFi,
    // pannello, configurazione del ricevitore) per quel tempo. Il ciclo
    // sotto prova comunque prima il WiFi; cellular_link_connect() torna
    // false finche' il modem non e' pronto e si ritenta al giro dopo.
    xTaskCreate(cellular_init_task, "cell_init", 6144, NULL, 4, NULL);

    // Ethernet e' indipendente dalla selezione WiFi/cellulare qui sopra:
    // se abilitata resta sempre attiva, usata soprattutto per raggiungere
    // il broadcast UDP NMEA anche via cavo. In un task dedicato (vedi
    // eth_link_init_task sopra), non chiamata qui direttamente - stack
    // overflow reale confermato altrimenti.
    xTaskCreate(eth_link_init_task, "eth_init", 6144, NULL, 5, NULL);

    // 4096 non bastava piu': wifi_link_connect_known() (per le reti WiFi
    // "conosciute") scansiona prima di collegarsi, e la scansione tiene
    // sullo stack un array di 32 wifi_ap_record_t (grosso, ~80-100 byte
    // l'uno) - confermato su hardware reale (stack overflow nel task
    // "net_manager" appena introdotta questa funzione). Stessa causa/fix
    // gia' vista piu' volte in questo progetto per lo stesso motivo.
    xTaskCreate(net_manager_task, "net_manager", 10240, NULL, 6, NULL); // 10 KB: margine per la scansione WiFi
}
