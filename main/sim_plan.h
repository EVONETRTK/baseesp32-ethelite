#pragma once

#include <stdint.h>
#include <stdbool.h>

// Rinnovo del piano della SIM: promemoria via email/WhatsApp (due, N giorni
// prima ciascuno, scelti nel pannello), avanzamento automatico della data
// per i rinnovi periodici, e periodo di conteggio del traffico allineato al
// rinnovo (l'operatore azzera i GB al rinnovo, non il 1 del mese).

#define SIM_RENEW_SINGLE  0   // scadenza unica (piano annuale, ricarica a mano)
#define SIM_RENEW_MONTHLY 1   // ogni mese nello stesso giorno
#define SIM_RENEW_DAYS    2   // ogni N giorni

// Inizio del periodo di conteggio che contiene il giorno "today" (aaaammgg)
// e sua durata in giorni. Senza rinnovo periodico: mese di calendario.
uint32_t sim_plan_period_start(uint32_t today);
uint32_t sim_plan_period_days(uint32_t period_start);

// Giorni al prossimo rinnovo (negativo = scaduto), false se non impostato
// o se l'ora non e' ancora nota.
bool sim_plan_days_left(int *days_left);

void sim_plan_start(void);
