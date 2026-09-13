#include "net_manager.h"
#include "wifi_link.h"
#include "cellular_link.h"
#include "eth_link.h"
#include "status.h"
#include "settings.h"

#include "sdkconfig.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_event.h"
#include "mdns.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "net_manager";

typedef enum {
    LINK_NONE,
    LINK_WIFI,
    LINK_CELLULAR,
} active_link_t;

static void net_manager_task(void *arg)
{
    active_link_t active = LINK_NONE;

    while (1) {
        app_settings_t settings = settings_get();

        switch (active) {
        case LINK_NONE:
            if (settings.network_mode != NETWORK_MODE_CELLULAR_ONLY) {
                ESP_LOGI(TAG, "Tentativo connessione WiFi...");
                if (wifi_link_connect(CONFIG_BASEESP32_WIFI_CONNECT_TIMEOUT_MS)) {
                    ESP_LOGI(TAG, "Rete attiva: WiFi");
                    status_set_net(NET_STATUS_WIFI);
                    active = LINK_WIFI;
                    break;
                }
                wifi_link_disconnect();
            }

            if (settings.network_mode != NETWORK_MODE_WIFI_ONLY) {
                ESP_LOGW(TAG, "Tentativo GPRS (SIM868)...");
                if (cellular_link_connect()) {
                    ESP_LOGI(TAG, "Rete attiva: cellulare (GPRS)");
                    status_set_net(NET_STATUS_CELLULAR);
                    active = LINK_CELLULAR;
                    break;
                }
            }

            status_set_net(NET_STATUS_NONE);
            ESP_LOGE(TAG, "Nessuna rete disponibile (l'AP di setup resta comunque attivo), nuovo tentativo tra %d ms",
                     CONFIG_BASEESP32_NET_RETRY_DELAY_MS);
            vTaskDelay(pdMS_TO_TICKS(CONFIG_BASEESP32_NET_RETRY_DELAY_MS));
            break;

        case LINK_WIFI:
            if (!wifi_link_is_connected()) {
                ESP_LOGW(TAG, "WiFi perso, ricomincio la selezione rete");
                wifi_link_disconnect();
                status_set_net(NET_STATUS_NONE);
                active = LINK_NONE;
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            break;

        case LINK_CELLULAR:
            if (!cellular_link_is_connected()) {
                ESP_LOGW(TAG, "GPRS perso, ricomincio la selezione rete (si ritenta prima il WiFi)");
                cellular_link_disconnect();
                status_set_net(NET_STATUS_NONE);
                active = LINK_NONE;
            } else {
                vTaskDelay(pdMS_TO_TICKS(3000));
            }
            break;
        }
    }
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
    app_settings_t settings = settings_get();
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
    wifi_link_init();
    cellular_link_init();

    // Ethernet e' indipendente dalla selezione WiFi/cellulare qui sopra:
    // se abilitata resta sempre attiva, usata soprattutto per raggiungere
    // il broadcast UDP NMEA anche via cavo.
    eth_link_init();

    xTaskCreate(net_manager_task, "net_manager", 4096, NULL, 6, NULL);
}
