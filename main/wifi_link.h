#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_netif.h"

// Inizializza il driver WiFi in modalita' station. Va chiamata dopo
// esp_netif_init()/esp_event_loop_create_default().
void wifi_link_init(void);

// Avvia il tentativo di connessione e blocca fino a timeout_ms o fino
// all'ottenimento di un IP. Ritorna true se connesso.
bool wifi_link_connect(uint32_t timeout_ms);

// Come wifi_link_connect(), ma applica prima SSID/password forniti (non
// quelli gia' salvati in settings) - permette alla UI web di testare una
// rete subito, senza dover salvare e riavviare il dispositivo per
// scoprire se le credenziali sono giuste.
bool wifi_link_connect_with(const char *ssid, const char *password, uint32_t timeout_ms);

// Scansiona le reti visibili e prova a collegarsi alla PRIMA rete "nota"
// (wifi_ssid principale, poi settings.wifi_known_networks - vedi
// settings.h) che risulta effettivamente visibile, preferendo tra quelle
// visibili quella col segnale migliore - permette di spostare il
// dispositivo tra piu' reti gia' provate con successo in passato (es.
// hotspot del campo, WiFi di casa) senza dover reinserire le credenziali:
// si ricollega da solo a qualunque rete nota trovi. Se nessuna rete nota
// e' visibile in questa scansione, ripiega comunque su un tentativo
// diretto sulla rete principale (utile se un AP non risponde alle probe
// request attive di una scansione ma accetterebbe una connessione
// diretta). Blocca per la durata di scansione + connessione.
bool wifi_link_connect_known(uint32_t connect_timeout_ms);

// Ferma il WiFi (usato prima di passare al fallback cellulare).
void wifi_link_disconnect(void);

bool wifi_link_is_connected(void);

// Netif station e AP (l'AP e' sempre valido dopo wifi_link_init(), la
// station anche se non connessa) - usati per il broadcast UDP NMEA.
esp_netif_t *wifi_link_get_sta_netif(void);
esp_netif_t *wifi_link_get_ap_netif(void);

// RSSI (dBm) dell'access point a cui la station e' connessa. Ritorna
// false se non connessa.
bool wifi_link_get_rssi(int8_t *rssi);

typedef struct {
    char ssid[33];
    int8_t rssi;
    bool secure; // false = rete aperta, senza password
} wifi_scan_result_t;

// Scansione WiFi bloccante (qualche secondo): copia fino a max_results
// reti trovate in out, deduplicate per SSID (tenendo il segnale
// migliore) e ordinate per segnale decrescente. Ritorna quante ne ha
// copiate. Interrompe brevemente il traffico AP durante la scansione -
// accettabile per un'azione di configurazione manuale, non da chiamare
// periodicamente.
size_t wifi_link_scan(wifi_scan_result_t *out, size_t max_results);
