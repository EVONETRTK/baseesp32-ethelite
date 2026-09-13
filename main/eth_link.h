#pragma once

#include <stdbool.h>
#include "esp_netif.h"

// Inizializza l'interfaccia Ethernet (chip W5500 via SPI, T-ETH-Elite), se
// abilitata in Kconfig (BASEESP32_ETHERNET_ENABLE). Va chiamata dopo
// esp_netif_init()/esp_event_loop_create_default(). A differenza della
// T-Internet-COM (LAN8720/RMII), qui tutti i pin (MISO/MOSI/SCLK/CS/INT)
// sono configurabili perche' e' un collegamento SPI, non vincolato a pin
// fissi dall'hardware ESP32. Non fa nulla se disabilitata.
void eth_link_init(void);

bool eth_link_is_connected(void);

// Netif Ethernet, o NULL se non abilitata/inizializzata - usato per il
// broadcast UDP NMEA anche su questa interfaccia.
esp_netif_t *eth_link_get_netif(void);
