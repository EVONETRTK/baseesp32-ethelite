#pragma once

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    NET_STATUS_NONE,
    NET_STATUS_WIFI,
    NET_STATUS_CELLULAR,
} net_status_t;

// Stato della connessione al caster NTRIP (base: carica RTCM; rover:
// scarica RTCM) - separato dal conteggio byte RTCM sopra, che dice se
// arrivano dati dalla UART GNSS ma non se il caster stesso e' raggiungibile.
// Protetto da mutex (a differenza degli altri campi scalari di questo
// file) perche' include un campo stringa, non scrivibile/leggibile in modo
// atomico.
typedef struct {
    bool connected;
    int64_t connected_since_us;  // 0 se non connesso
    int64_t last_disconnect_us;  // 0 se mai disconnesso dopo un avvio pulito
    uint32_t connect_count;      // quante volte si e' connesso con successo dal boot (utile per notare instabilita')
    char last_error[80];         // motivo dell'ultimo errore/disconnessione, stringa vuota se nessuno finora
} ntrip_conn_status_t;

// Da chiamare subito dopo un handshake NTRIP riuscito.
void status_ntrip_note_connected(void);

// Da chiamare quando la connessione al caster cade o fallisce, con una
// breve descrizione del motivo (troncata se piu' lunga del campo
// last_error).
void status_ntrip_note_disconnected(const char *reason);

ntrip_conn_status_t status_ntrip_get(void);

// Stato condiviso, letto dalla UI web e aggiornato dagli altri task.
// Non e' protetto da lock: le singole letture/scritture sono variabili
// scalari, sufficiente per un pannello di stato non critico.

void status_set_net(net_status_t s);
net_status_t status_get_net(void);

void status_note_rtcm_bytes(uint32_t n);
uint32_t status_get_rtcm_total_bytes(void);

// Timestamp (microsecondi, esp_timer_get_time) dell'ultimo dato RTCM
// ricevuto dalla UART GNSS; 0 se non ancora ricevuto nulla.
int64_t status_get_last_rtcm_time_us(void);

// Speculare a rtcm_bytes/last_rtcm_us sopra ma per la direzione opposta
// (solo rover): l'ultima riga $GxGGA inoltrata al caster, per mostrare in
// UI un collegamento NTRIP come davvero bidirezionale (correzioni in
// arrivo E posizione in uscita), non solo "connesso si/no".
void status_note_gga_sent(void);
int64_t status_get_last_gga_sent_time_us(void);

// Timestamp dell'ultimo controllo aggiornamenti online riuscito (arrivata
// una risposta valida dal manifest, disponibile o no che fosse una nuova
// versione - non conta un tentativo fallito per rete assente). 0 se mai
// avvenuto dal boot. Aggiornato sia dal controllo manuale ("Controlla
// aggiornamenti online" in Firmware) sia da quello automatico
// (auto_update.c) - la UI lo usa per avvisare se il controllo automatico,
// pur attivo, non riesce a completarsi da troppo tempo (es. problema di
// rete persistente che altrimenti si scoprirebbe solo guardando i log).
void status_note_online_update_checked(void);
int64_t status_get_last_online_update_check_us(void);

// Stato INS/prua per GNSS_CHIP_BYNAV_M21D (vedi gnss_bynav_m21d_configure_
// rover() e gnss_nmea_reader.c, che parsano le righe #INSPVAXA/#HEADINGA
// dalla UART e chiamano status_bynav_ins_note()) - ignorato/sempre a zero
// per qualunque altro chip GNSS. ins_status e' una delle stringhe della
// state machine di allineamento INS (INS_INACTIVE...INS_SOLUTION_GOOD, vedi
// Tabella 1-3 di AN065); heading/pitch/roll in gradi, validi solo quando
// have_attitude e' true (impostato al primo #INSPVAXA/#HEADINGA ricevuto
// dopo l'avvio). Protetto da mutex per lo stesso motivo di ntrip_conn_
// status_t sopra (contiene un campo stringa).
typedef struct {
    bool have_attitude;
    char ins_status[24];   // es. "INS_SOLUTION_GOOD", "INS_ALIGNING" - vedi Tabella 1-3 AN065
    float heading_deg;
    float pitch_deg;
    float roll_deg;
    int64_t last_update_us;
} bynav_ins_status_t;

// Da chiamare quando si riceve una riga #INSPVAXA valida dalla UART (vedi
// gnss_nmea_reader.c): porta lo stato di allineamento INS (campo 1) e
// roll/pitch/azimuth (campi 10/11/12, formato compatibile NovAtel OEM7 -
// stessi nomi di log riusati da Bynav, non verificato su un modulo M21D
// reale). azimuth qui e' preso come "heading".
void status_bynav_ins_note_inspvaxa(const char *ins_status, float roll_deg, float pitch_deg, float heading_deg);

// Da chiamare quando si riceve una riga #HEADINGA valida dalla UART: porta
// solo heading/pitch (campi 4/5), nessuno stato di allineamento - non
// tocca ins_status gia' salvato da INSPVAXA.
void status_bynav_ins_note_headinga(float heading_deg, float pitch_deg);

bynav_ins_status_t status_bynav_ins_get(void);

// Modalita' (base/rover) con cui il firmware e' partito a questo avvio: la
// UI la confronta con quella salvata (settings.device_mode), che cambia
// subito al salvataggio ma si applica solo dopo il riavvio. true = rover.
void status_set_active_rover(bool rover);
bool status_get_active_rover(void);
