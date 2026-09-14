#pragma once

#include <stddef.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"

// Registra uno stream buffer aggiuntivo su cui inoltrare (in copia, non
// bloccante: righe perse se il buffer e' pieno, senza impatto sul log
// principale su UART/RAM) ogni riga di log gia' intercettata da questo
// modulo - usato da diag_log.c per scriverle anche su SD, senza dover
// installare un secondo hook esp_log_set_vprintf() (ce n'e' solo uno
// attivo alla volta in ESP-IDF). Passare NULL per disattivare.
void log_buffer_set_sink(StreamBufferHandle_t sink);

// Cattura una copia degli ultimi log (ESP_LOGx) in un buffer circolare in
// RAM, oltre a continuare a stamparli sulla UART come sempre - permette di
// vederli dalla scheda "Log" della UI web senza bisogno di un cavo
// seriale, utile soprattutto sul campo. Va chiamata una volta, il prima
// possibile in app_main(), per non perdere i log di avvio.
void log_buffer_init(void);

// Copia gli ultimi log disponibili (i piu' recenti, se il buffer e' pieno)
// in out, fino a max_len byte (non termina con '\0' da sola: il chiamante
// deve lasciare spazio se vuole aggiungerlo). Ritorna quanti byte ha
// copiato. Puo' essere chiamata da qualunque task (es. il server web).
size_t log_buffer_read(char *out, size_t max_len);
