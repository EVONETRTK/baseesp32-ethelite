#include "nmea_udp_broadcast.h"
#include "settings.h"
#include "wifi_link.h"
#include "eth_link.h"

#include <sys/socket.h>
#include <netinet/in.h>
#include <string.h>

#include "esp_log.h"
#include "esp_netif.h"

static const char *TAG = "nmea_udp";
static int s_sock = -1;

void nmea_udp_broadcast_init(void)
{
    s_sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (s_sock < 0) {
        ESP_LOGE(TAG, "Creazione socket UDP fallita");
        return;
    }
    int broadcast_enable = 1;
    setsockopt(s_sock, SOL_SOCKET, SO_BROADCAST, &broadcast_enable, sizeof(broadcast_enable));
}

static void broadcast_to_netif(esp_netif_t *netif, const char *line, size_t len, uint16_t port)
{
    if (!netif || !esp_netif_is_netif_up(netif)) {
        return;
    }

    esp_netif_ip_info_t ip_info;
    if (esp_netif_get_ip_info(netif, &ip_info) != ESP_OK || ip_info.ip.addr == 0) {
        return;
    }

    uint32_t bcast_addr = (ip_info.ip.addr & ip_info.netmask.addr) | ~ip_info.netmask.addr;

    struct sockaddr_in dest = { 0 };
    dest.sin_family = AF_INET;
    dest.sin_port = htons(port);
    dest.sin_addr.s_addr = bcast_addr;

    sendto(s_sock, line, len, 0, (struct sockaddr *) &dest, sizeof(dest));
}

void nmea_udp_broadcast_send(const char *line, size_t len)
{
    if (s_sock < 0) {
        return;
    }

    uint16_t port = settings_get().nmea_udp_port;

    broadcast_to_netif(wifi_link_get_ap_netif(), line, len, port);
    broadcast_to_netif(wifi_link_get_sta_netif(), line, len, port);
    broadcast_to_netif(eth_link_get_netif(), line, len, port);
}
