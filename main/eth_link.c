#include "eth_link.h"
#include "sdkconfig.h"

#include "esp_log.h"
#include "esp_event.h"
#include "esp_netif.h"
#include "esp_mac.h"

static const char *TAG = "eth_link";

#if CONFIG_BASEESP32_ETHERNET_ENABLE

#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_mac_spi.h" // eth_w5500_config_t/ETH_W5500_DEFAULT_CONFIG/esp_eth_mac_new_w5500: non arrivano da esp_eth_mac.h
#include "esp_eth_phy.h"
#include "driver/spi_master.h"

static esp_netif_t *s_eth_netif = NULL;
static volatile bool s_connected = false;

static void on_eth_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        ESP_LOGI(TAG, "Link Ethernet su");
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGW(TAG, "Link Ethernet giu'");
        s_connected = false;
        break;
    default:
        break;
    }
}

static void on_ip_event(void *arg, esp_event_base_t event_base, int32_t event_id, void *event_data)
{
    if (event_id == IP_EVENT_ETH_GOT_IP) {
        ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
        ESP_LOGI(TAG, "Ethernet IP ottenuto: " IPSTR, IP2STR(&event->ip_info.ip));
        s_connected = true;
    }
}

void eth_link_init(void)
{
    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    s_eth_netif = esp_netif_new(&netif_cfg);
    if (!s_eth_netif) {
        ESP_LOGE(TAG, "Creazione netif Ethernet fallita");
        return;
    }

    spi_bus_config_t buscfg = {
        .miso_io_num = CONFIG_BASEESP32_ETH_SPI_MISO_PIN,
        .mosi_io_num = CONFIG_BASEESP32_ETH_SPI_MOSI_PIN,
        .sclk_io_num = CONFIG_BASEESP32_ETH_SPI_SCLK_PIN,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
    };
    if (spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO) != ESP_OK) {
        ESP_LOGE(TAG, "Init bus SPI per W5500 fallita");
        return;
    }

    spi_device_interface_config_t devcfg = {
        .command_bits = 16,
        .address_bits = 8,
        .mode = 0,
        .clock_speed_hz = 20 * 1000 * 1000, // 20MHz, valore tipico per W5500
        .queue_size = 20,
        .spics_io_num = CONFIG_BASEESP32_ETH_SPI_CS_PIN,
    };

    eth_w5500_config_t w5500_config = ETH_W5500_DEFAULT_CONFIG(SPI2_HOST, &devcfg);
    w5500_config.int_gpio_num = CONFIG_BASEESP32_ETH_SPI_INT_PIN;

    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    esp_eth_mac_t *mac = esp_eth_mac_new_w5500(&w5500_config, &mac_config);

    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = CONFIG_BASEESP32_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = CONFIG_BASEESP32_ETH_PHY_RST_PIN;
    esp_eth_phy_t *phy = esp_eth_phy_new_w5500(&phy_config);

    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    esp_eth_handle_t eth_handle = NULL;
    if (esp_eth_driver_install(&eth_config, &eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Init driver Ethernet (W5500) fallita - verificare pin SPI/indirizzo PHY "
                      "contro lo schema elettrico della scheda (mai testato su hardware reale)");
        return;
    }

    // Il W5500 non ha un MAC address di fabbrica come i PHY RMII interni
    // all'ESP32: va assegnato esplicitamente. Deriviamo un indirizzo
    // valido dal MAC efuse dell'ESP32 stesso (prassi standard ESP-IDF per
    // Ethernet SPI esterna).
    uint8_t mac_addr[6];
    esp_read_mac(mac_addr, ESP_MAC_ETH);
    esp_eth_ioctl(eth_handle, ETH_CMD_S_MAC_ADDR, mac_addr);

    esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(eth_handle);
    esp_netif_attach(s_eth_netif, glue);

    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &on_eth_event, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &on_ip_event, NULL));

    if (esp_eth_start(eth_handle) != ESP_OK) {
        ESP_LOGE(TAG, "Avvio Ethernet fallito");
    }
}

bool eth_link_is_connected(void)
{
    return s_connected;
}

esp_netif_t *eth_link_get_netif(void)
{
    return s_eth_netif;
}

#else // !CONFIG_BASEESP32_ETHERNET_ENABLE

void eth_link_init(void) { }
bool eth_link_is_connected(void) { return false; }
esp_netif_t *eth_link_get_netif(void) { return NULL; }

#endif
