#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <time.h>
#include "settings.h"

// Gestione della SIM dal pannello: credito (USSD o SMS), SMS liberi,
// lettura e cancellazione degli SMS ricevuti, controllo automatico del
// credito con avviso sotto una soglia. Nessun task proprio: le operazioni
// sul modem le esegue il task degli avvisi (alerts.c), svegliato subito
// quando arriva una richiesta dal pannello.

typedef enum {
    SIM_ACT_NONE = 0,
    SIM_ACT_CREDIT,     // con il modo e il codice/SMS delle impostazioni
    SIM_ACT_USSD,       // codice scritto al momento (a)
    SIM_ACT_SMS,        // numero (a) e testo (b) scritti al momento
    SIM_ACT_READ_SMS,
    SIM_ACT_DELETE_SMS,
    SIM_ACT_FIND_NUMBER, // SMS di prova al numero (a): il mittente e' il numero della SIM
} sim_action_t;

// false (con motivo in err) se un'altra operazione e' in corso.
bool sim_tools_request(sim_action_t action, const char *a, const char *b, char *err, size_t err_size);

// Dal task degli avvisi a ogni giro: esegue le richieste e il controllo
// automatico.
void sim_tools_tick(const app_settings_t *cfg);

// Legge un importo in euro da una risposta dell'operatore ("Credito
// residuo 7,50 euro", "Saldo: EUR 3.20"...). false se non lo trova.
bool sim_tools_parse_euro(const char *text, float *eur);

typedef struct {
    bool busy;                 // operazione in corso
    char last_msg[160];        // esito dell'ultima operazione, per il pannello
    time_t credit_at;          // ultimo credito letto (0 = mai)
    char credit_text[200];
    bool credit_eur_valid;
    float credit_eur;
    char iccid[24];            // dal modem; dopo un riavvio, l'ultimo letto (salvato)
    char phone[20];            // dal modem (AT+CNUM), spesso vuoto
    char imei[20];             // dal modem (AT+CGSN); come iccid
    bool iccid_live;           // iccid letto dal modem dopo l'ultimo avvio
    bool imei_live;
    time_t ids_at;             // ultima lettura di ICCID/IMEI (0 = mai)
    char iccid_prev[24];       // SIM di prima, se all'avvio e' diversa (SIM cambiata)
    char imei_prev[20];        // modem di prima, se all'avvio e' diverso (modem sostituito)
    bool number_written;       // numero scritto a mano copiato nella SIM in questo avvio
} sim_tools_status_t;

void sim_tools_get_status(sim_tools_status_t *out);

// SMS ricevuti, risposta grezza di AT+CMGL (il pannello la interpreta).
// Copiata in out; stringa vuota se non ancora letti.
void sim_tools_get_sms(char *out, size_t out_size);

void sim_tools_start(void);
