#pragma once

#include <stdbool.h>
#include <stddef.h>

// Uscita delle frasi NMEA via Bluetooth Low Energy (1.32.0) per le app del
// telefono/tablet (es. SW Maps): servizio "Nordic UART" (NUS), le frasi
// arrivano come notifiche sulla caratteristica TX. Solo in rover, solo se
// l'impostazione ble_nmea_enable e' accesa (vale dopo il riavvio). Il chip
// (ESP32-S3) non ha il Bluetooth classico: le app che lo richiedono non
// vedono il ricevitore.

typedef enum {
    NMEA_BLE_OFF = 0,   // spento (impostazione spenta, oppure base)
    NMEA_BLE_ON = 1,    // annuncio/connessione attivi
    NMEA_BLE_ERROR = 2, // avvio fallito (di solito memoria): il resto funziona
} nmea_ble_state_t;

// Da chiamare UNA sola volta all'avvio, prima di qualunque uso del
// Bluetooth: se l'uscita BLE non servira' (spenta o base) restituisce al
// sistema la memoria riservata al Bluetooth (esp_bt_mem_release); dopo,
// nmea_ble_start() non fa piu' nulla.
void nmea_ble_boot(bool will_start);

// Avvia NimBLE con il nome annunciato 'name' (rete di setup, es.
// "EVONETRTK-123456"). Non blocca e non abortisce: se fallisce lo stato
// diventa NMEA_BLE_ERROR.
void nmea_ble_start(const char *name);

// Accoda una frase NMEA (senza CR/LF) per il telefono collegato. Mai
// bloccante: senza telefono iscritto alle notifiche, o con la coda piena,
// la frase si scarta. Chiamata dal task che legge il ricevitore.
void nmea_ble_send(const char *line, size_t len);

nmea_ble_state_t nmea_ble_state(void);
const char *nmea_ble_state_str(nmea_ble_state_t st); // "attivo"/"spento"/"errore"
bool nmea_ble_client_connected(void);

// Esito della restituzione della memoria del Bluetooth all'avvio (diagnosi).
void nmea_ble_mem_info(int *result, int *released_bytes);
