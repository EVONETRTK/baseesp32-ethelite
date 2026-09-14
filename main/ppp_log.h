#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#include <stdio.h>

// Registra su microSD lo stream RTCM3 grezzo generato dal GNSS in modalita'
// base (stesso flusso gia' inoltrato al caster/monitor spostamento) in un
// unico file (/sdcard/ppp_log.rtcm3, sovrascritto ad ogni nuovo avvio di
// registrazione) - serve a raccogliere ore di osservazioni da un punto
// fisso, da convertire poi (a scelta e cura dell'utente, MAI in automatico)
// in RINEX e inviare a un servizio di post-processing PPP (CSRS-PPP, OPUS,
// AUSPOS...) per ottenere una posizione assoluta centimetrica - vedi
// "Posizione base" in settings.h. La registrazione parte/si ferma solo su
// richiesta esplicita dalla UI web, mai da sola.
//
// Il file grezzo si converte in RINEX (nella versione richiesta da
// ciascun ente) con lo strumento gratuito convbin di RTKLIB, sul proprio
// PC, ad es.:
//   convbin ppp_log.rtcm3 -r rtcm3 -v 3.03 -n ppp_log_v3.obs   (RINEX 3.03)
//   convbin ppp_log.rtcm3 -r rtcm3 -v 2.11 -n ppp_log_v2.obs   (RINEX 2.11)
// Un solo file grezzo basta a generare il formato richiesto da ciascun
// servizio - non serve rifare la registrazione per ognuno.

// Avvia una nuova registrazione (monta la SD e apre il file, troncando
// un'eventuale registrazione precedente non ancora scaricata). Ritorna
// false se la SD non e' presente/montabile. Se una registrazione e' gia'
// in corso non fa nulla e ritorna true.
bool ppp_log_start(void);

// Ferma la registrazione in corso (chiude il file, smonta la SD) - il file
// resta disponibile per il download finche' non se ne avvia una nuova.
// Sicura da chiamare anche se non c'e' nessuna registrazione in corso.
void ppp_log_stop(void);

typedef struct {
    bool recording;
    uint64_t bytes_written;
    int64_t started_at_us;  // esp_timer_get_time() all'avvio, valido se recording o file_exists
    bool file_exists;       // un file (in corso o completato) e' disponibile per il download
} ppp_log_status_t;

ppp_log_status_t ppp_log_get_status(void);

// Da chiamare ogni volta che arrivano nuovi byte RTCM3 dal GNSS - non
// bloccante (niente I/O su SD in questo task: scrive solo su uno stream
// buffer interno, consumato da un task separato), per non rallentare il
// chiamante (gnss_uart_task, sul percorso critico verso il caster). Non fa
// nulla se non e' in corso nessuna registrazione.
void ppp_log_feed(const uint8_t *data, size_t len);

// Apre in lettura il file dell'ultima registrazione (monta la SD) - da
// usare SOLO per servire il download via HTTP, e SOLO quando nessuna
// registrazione e' in corso (altrimenti la SD resta occupata in scrittura
// dal task di registrazione e questa chiamata fallisce). Il chiamante deve
// richiamare ppp_log_close_for_read() con lo stesso FILE* quando ha
// finito, per smontare correttamente la SD.
FILE *ppp_log_open_for_read(void);
void ppp_log_close_for_read(FILE *f);
