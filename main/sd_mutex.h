#pragma once

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

// Mutex condiviso tra TUTTI i moduli che montano/smontano la microSD
// (sd_update.c, diag_log.c, fw_archive.c, ppp_log.c) - ognuno usa lo
// stesso bus SPI/slot (SPI3_HOST) per la scheda, ma prima di questo
// mutex ogni modulo montava/smontava per conto proprio senza sapere
// degli altri: se due capitavano a montare nello stesso istante, il
// secondo falliva silenziosamente (spi_bus_initialize/mount fallivano,
// "archiviazione saltata" e simili) invece di aspettare il proprio
// turno. Confermato su hardware reale: l'archiviazione automatica del
// firmware su SD all'avvio falliva sistematicamente per questo esatto
// motivo, contro diag_log che monta/smonta ogni 30s per tutta la vita
// del dispositivo.
//
// RICORSIVO deliberatamente: sd_update_check_and_apply_impl() tiene il
// mutex per tutta la sua durata e al suo interno chiama
// ota_update_apply(), che a sua volta chiama fw_archive_save_current()
// (per archiviare la versione uscente prima di sovrascriverla) - stesso
// task, secondo giro di take() prima del primo give(). Un mutex normale
// si bloccherebbe da solo in questo caso (deadlock reale, confermato
// leggendo il percorso di chiamata prima di scegliere il tipo di
// mutex); quello ricorsivo permette allo stesso task di riprenderlo piu'
// volte, richiedendo lo stesso numero di give() per rilasciarlo davvero.
//
// Creato UNA VOLTA, in modo sincrono, prima che qualunque task che usa
// la SD possa partire (vedi sd_mutex_init() in main.c) - stessa tecnica
// gia' usata per gli altri mutex "presto" di questo progetto (vedi
// settings.c), per evitare la finestra in cui due task vedrebbero
// entrambi il mutex non ancora creato.
void sd_mutex_init(void);

// Preso PRIMA di montare la SD, rilasciato DOPO averla smontata - vedi
// ogni modulo per l'uso esatto. portMAX_DELAY: aspettare il proprio
// turno e' sempre corretto qui (nessun modulo tiene la SD montata a
// lungo), non c'e' un valore utile per un timeout piu' breve. Puo'
// essere richiamato dallo stesso task che lo tiene gia' (vedi sopra) -
// ogni take() va abbinato a un give(), anche quando annidati.
void sd_mutex_take(void);
void sd_mutex_give(void);
