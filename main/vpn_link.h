#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// Accesso remoto via VPN WireGuard (componente trombik/esp_wireguard).
// La base apre un tunnel verso un server WireGuard (es. il server del
// caster) e ci resta collegata con un keepalive ogni 25 s: funziona anche
// dietro il CGNAT della SIM, perche' e' la base a chiamare il server. Chi
// e' collegato alla stessa VPN (PC o telefono con l'app WireGuard) apre il
// pannello all'indirizzo della base nel tunnel (es. http://10.8.0.2).
//
// Il tunnel serve solo per raggiungere la base: non diventa la rete
// predefinita, RTCM e caster restano sulla rete normale (WiFi/SIM/Ethernet).
// La chiave privata la genera la base (vpn_link_generate_keys) e non esce
// mai: nel pannello si vede solo la chiave pubblica, da mettere sul server.

// Da chiamare periodicamente dal ciclo della rete (net_manager.c): avvia,
// controlla e riavvia il tunnel secondo impostazioni e stato della rete.
void vpn_link_tick(void);

// Genera una nuova coppia di chiavi e salva la privata nelle impostazioni.
// Scrive in pub_b64 la chiave pubblica (base64, 44 caratteri + zero).
bool vpn_link_generate_keys(char *pub_b64, size_t pub_size);

typedef struct {
    bool enabled;      // attivato nelle impostazioni
    bool configured;   // chiavi, server e indirizzo presenti
    bool started;      // interfaccia del tunnel creata
    bool up;           // handshake riuscito con il server (tunnel funzionante)
    int64_t up_since_us;
    int64_t last_up_us; // ultima volta visto funzionante
    char public_key[48]; // chiave pubblica della base ("" se non generata)
    char last_error[64];
} vpn_status_t;

void vpn_link_get_status(vpn_status_t *out);
