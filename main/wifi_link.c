#include "wifi_link.h"
#include "settings.h"

#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

#include "esp_log.h"
#include "esp_wifi.h"
#include "esp_netif.h"
#include "esp_event.h"

static const char *TAG = "wifi_link";

#define WIFI_CONNECTED_BIT BIT0

static EventGroupHandle_t s_events;
static volatile bool s_connected = false;
static volatile bool s_should_reconnect = false;
static esp_netif_t *s_sta_netif = NULL;
static esp_netif_t *s_ap_netif = NULL;

static void wifi_event_handler(void *arg, esp_event_base_t event_base,
                                int32_t event_id, void *event_data)
{
    if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
        s_connected = false;
        xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
        if (s_should_reconnect) {
            esp_wifi_connect();
        }
    } else if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "STA IP ottenuto: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
        xEventGroupSetBits(s_events, WIFI_CONNECTED_BIT);
    } else if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_AP_STACONNECTED) {
        ESP_LOGI(TAG, "Client connesso all'AP di setup");
    }
}

void wifi_link_init(void)
{
    s_events = xEventGroupCreate();

    s_sta_netif = esp_netif_create_default_wifi_sta();
    s_ap_netif = esp_netif_create_default_wifi_ap();

    wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&cfg));

    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, NULL));

    app_settings_t settings = settings_get();

    // L'AP di setup resta sempre attivo (modalita' APSTA): la UI web e'
    // cosi' raggiungibile in campo anche se WiFi/GPRS non sono ancora
    // configurati o non funzionano.
    wifi_config_t ap_config = { 0 };
    strncpy((char *) ap_config.ap.ssid, settings.ap_ssid, sizeof(ap_config.ap.ssid) - 1);
    ap_config.ap.ssid_len = (uint8_t) strlen(settings.ap_ssid);
    ap_config.ap.channel = 1;
    ap_config.ap.max_connection = 4;
    if (strlen(settings.ap_password) >= 8) {
        strncpy((char *) ap_config.ap.password, settings.ap_password, sizeof(ap_config.ap.password) - 1);
        ap_config.ap.authmode = WIFI_AUTH_WPA2_PSK;
    } else {
        ap_config.ap.authmode = WIFI_AUTH_OPEN;
    }

    wifi_config_t sta_config = { 0 };
    strncpy((char *) sta_config.sta.ssid, settings.wifi_ssid, sizeof(sta_config.sta.ssid) - 1);
    strncpy((char *) sta_config.sta.password, settings.wifi_password, sizeof(sta_config.sta.password) - 1);
    sta_config.sta.threshold.authmode = WIFI_AUTH_WPA2_PSK;

    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap_config));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &sta_config));
    ESP_ERROR_CHECK(esp_wifi_start());

    ESP_LOGI(TAG, "AP di setup attivo: SSID=%s IP=192.168.4.1", settings.ap_ssid);
}

bool wifi_link_connect(uint32_t timeout_ms)
{
    xEventGroupClearBits(s_events, WIFI_CONNECTED_BIT);
    s_should_reconnect = true;

    esp_err_t err = esp_wifi_connect();
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "esp_wifi_connect fallito: %s", esp_err_to_name(err));
        return false;
    }

    EventBits_t bits = xEventGroupWaitBits(s_events, WIFI_CONNECTED_BIT,
                                            pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    return (bits & WIFI_CONNECTED_BIT) != 0;
}

void wifi_link_disconnect(void)
{
    s_should_reconnect = false;
    esp_wifi_disconnect();
    s_connected = false;
}

bool wifi_link_is_connected(void)
{
    return s_connected;
}

esp_netif_t *wifi_link_get_sta_netif(void)
{
    return s_sta_netif;
}

esp_netif_t *wifi_link_get_ap_netif(void)
{
    return s_ap_netif;
}

bool wifi_link_get_rssi(int8_t *rssi)
{
    if (!s_connected) {
        return false;
    }
    wifi_ap_record_t info;
    if (esp_wifi_sta_get_ap_info(&info) != ESP_OK) {
        return false;
    }
    *rssi = info.rssi;
    return true;
}
