#pragma once

#include <stdint.h>
#include <stdbool.h>

// Inizializza lo stato condiviso di riconoscimento ACK/NAK UBX. Va chiamata
// una volta all'avvio prima di usare le altre funzioni di questo modulo.
void gnss_ubx_ack_init(void);

// Modello e firmware del ricevitore u-blox, dalle righe "MOD=" e "FWVER="
// di UBX-MON-VER (letto all'avvio). "" se non ancora ricevuti.
const char *gnss_ubx_ack_model(void);
const char *gnss_ubx_ack_fw(void);

// Da chiamare subito PRIMA di spedire un comando UBX di cui si attende la
// risposta (vedi ubx_send() in gnss_ubx.c): scarta un eventuale ACK residuo
// e attiva il riconoscimento dei frame UBX sullo stream della UART. Fuori
// dalla finestra aperta qui e chiusa da gnss_ubx_ack_wait() il parser resta
// spento e nessun byte viene consumato: lo stream RTCM3 binario della base
// contiene 0xB5 e 0xB5 0x62 per puro caso, e riconoscerli sempre voleva
// dire mangiare byte RTCM validi.
void gnss_ubx_ack_arm(void);

// Da chiamare per OGNI byte letto dalla UART del ricevitore, prima di
// considerarlo per il parsing NMEA (vedi gnss_nmea_reader.c) - riconosce i
// frame binari UBX-ACK-ACK/UBX-ACK-NAK (sync 0xB5 0x62, mai presente in
// NMEA puro ASCII) e li accantona. Attivo solo nella finestra aperta da
// gnss_ubx_ack_arm(). Ritorna true se il byte e' stato consumato come parte
// di un frame UBX in corso - il chiamante non deve piu' passarlo al parser
// NMEA.
bool gnss_ubx_ack_feed_byte(uint8_t c);

// Attende (bloccante, con timeout) l'ACK/NAK per l'ultimo comando
// UBX-CFG-VALSET mandato, poi chiude la finestra aperta da
// gnss_ubx_ack_arm(). Va chiamata subito dopo aver spedito il comando
// (nessun'altra attesa deve essere in corso nel frattempo: un solo comando
// alla volta, mai in parallelo). Ritorna false se scade il timeout senza
// risposta - anche questa e' un'informazione utile: puo' voler dire che la
// UART e' sbagliata o che il ricevitore non implementa affatto il
// protocollo di configurazione atteso. *out_acked, se il ritorno e' true,
// indica se il comando e' stato accettato (true) o rifiutato/NAK (false).
// *out_echo_cls/*out_echo_id (se non NULL) riportano la classe/id che
// l'ACK/NAK dichiara di star confermando/rifiutando - permette di
// verificare che la risposta si riferisca davvero al comando appena
// mandato, invece di darlo per scontato.
bool gnss_ubx_ack_wait(uint32_t timeout_ms, bool *out_acked, uint8_t *out_echo_cls, uint8_t *out_echo_id);
