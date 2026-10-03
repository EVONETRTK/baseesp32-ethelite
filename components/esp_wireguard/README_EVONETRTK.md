# esp_wireguard (copia modificata)

Copia di trombik/esp_wireguard 0.9.0 (licenza nel file LICENSE), tenuta nel
progetto perché modificata (03/10/2026):

- le chiamate a lwIP (creazione dell'interfaccia, peer, collegamento,
  chiusura, stato) girano nel thread tcpip con esp_netif_tcpip_exec();
- la risoluzione DNS del server avviene prima, fuori da quel thread.

Prima venivano eseguite dal task dell'applicazione, senza core locking: una
corsa con lwIP all'apertura e alla chiusura del tunnel.
