#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "modem_health.h"

// Inizializza il modem SIM7600 (UART + netif PPP tramite esp_modem). Va
// chiamata dopo esp_netif_init()/esp_event_loop_create_default(). Ritorna
// false se il supporto cellulare e' disabilitato in Kconfig o se
// l'inizializzazione fallisce.
bool cellular_link_init(void);

// Avvia la connessione dati (PPP) e blocca fino all'ottenimento di un IP
// o a un timeout interno. Ritorna false se il supporto e' disabilitato,
// non inizializzato, o la connessione fallisce.
bool cellular_link_connect(void);

// Riporta il modem in modalita' comando (chiude la sessione dati PPP).
void cellular_link_disconnect(void);

bool cellular_link_is_connected(void);

// Primo avvio del modem concluso (riuscito o no): prima, un tentativo di
// collegamento fallisce comunque.
bool cellular_link_ready(void);

// Errori della seriale del modem dall'avvio (ognuno fa ripartire il CMUX):
// net_manager richiude il 4G se crescono troppo in fretta.
uint32_t cellular_link_serial_errors(void);
// Stessi errori per tipo: [0] FIFO o buffer pieni, [1] Rx Break o formato, [2] parita'.
void cellular_link_serial_error_counts(uint32_t out[3]);

// Qualita' del segnale (comando AT+CSQ). rssi_dbm e' gia' convertito in
// dBm dalla scala CSQ 0-31 (formula standard: -113 + 2*csq). Ritorna
// false se il supporto cellulare e' disabilitato, il modem non e'
// inizializzato, o la lettura fallisce (es. nessuna copertura).
bool cellular_link_get_signal(int *rssi_dbm);

// Nome operatore e tecnologia di accesso correnti (comando AT+COPS?). La
// tecnologia e' derivata dal codice standard 3GPP "AcT" e mappata su
// 2G/3G/4G/5G in modo generico. Ritorna false se il supporto e'
// disabilitato, il modem non e' inizializzato, o la lettura fallisce
// (es. non ancora registrato in rete).
bool cellular_link_get_operator_info(char *operator_out, size_t operator_out_size,
                                      char *tech_out, size_t tech_out_size);

// Gestione SIM (sim_tools.c), solo dal task degli avvisi. out contiene la
// risposta grezza del modem.
bool cellular_link_modem_present(void);

// Riavvio del modem quando non riesce piu' a collegarsi (modem "piantato":
// capita ai moduli LTE dopo giorni accesi). Prima AT+CFUN=1,1; se il modem
// non risponde nemmeno ai comandi, spegnimento e riaccensione col pin PWRKEY
// (non con il modulo SIMCom originale, che ha PWRKEY scollegato). Bloccante
// (fino a ~30 s), solo dal task della rete con il collegamento dati chiuso.
bool cellular_link_reset_modem(void);
bool cellular_link_get_iccid(char *out, size_t out_size);

// Numero di telefono scritto nella SIM (AT+CNUM). false se il modem non
// risponde; true con stringa vuota se la SIM non lo contiene (frequente).
bool cellular_link_get_number(char *out, size_t out_size);

// IMEI del modem (AT+CGSN), anche senza SIM inserita.
bool cellular_link_get_imei(char *out, size_t out_size);

// Scrive il numero nella SIM (rubrica dei numeri propri, AT+CPBW), cosi'
// AT+CNUM lo restituisce anche in un'altra base. number: solo cifre e +
// iniziale. false se la SIM non lo accetta.
bool cellular_link_write_number(const char *number);

// Codice USSD: solo cifre, '*', '#', '+'. Numeri: '+' facoltativo e 3-20
// cifre; testo SMS senza caratteri di controllo. Altrimenti false (finirebbero
// dentro un comando AT). Con il collegamento dati attivo senza CMUX (SIM868)
// i comandi SIM falliscono subito invece di aspettare il timeout.
bool cellular_link_ussd(const char *code, char *out, size_t out_size);
bool cellular_link_send_sms(const char *number, const char *text);
bool cellular_link_read_sms(char *out, size_t out_size);
bool cellular_link_delete_sms(void);

// Salute del modem (temperatura, tensione, AT+CPSI?, con fw=true anche la
// versione del firmware). Come i comandi SIM: solo dal task di sim_tools.c.
// Campi non letti restano *_ok = false. false se il modem non risponde.
bool cellular_link_read_health(modem_health_t *h, bool fw);
