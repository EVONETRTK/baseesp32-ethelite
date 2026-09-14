#include "log_buffer.h"

#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include <stdbool.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define LOG_BUFFER_CAPACITY 8192

static char s_buf[LOG_BUFFER_CAPACITY];
static size_t s_head;      // prossima posizione di scrittura
static bool s_wrapped;     // true se il buffer ha gia' fatto almeno un giro
static SemaphoreHandle_t s_mutex;
static vprintf_like_t s_orig_vprintf;
static StreamBufferHandle_t s_sink;

void log_buffer_set_sink(StreamBufferHandle_t sink)
{
    s_sink = sink;
}

static int log_buffer_vprintf(const char *fmt, va_list args)
{
    // Il log va comunque sulla UART come sempre (comportamento invariato
    // per chi usa il cavo seriale) - va fatto con una copia separata di
    // va_list perche' vprintf "consuma" quella originale, e la stessa
    // args serve ancora sotto per vsnprintf().
    va_list args_copy;
    va_copy(args_copy, args);
    int ret = s_orig_vprintf ? s_orig_vprintf(fmt, args_copy) : vprintf(fmt, args_copy);
    va_end(args_copy);

    if (!s_mutex) {
        return ret;
    }

    char line[256];
    int n = vsnprintf(line, sizeof(line), fmt, args);
    if (n <= 0) {
        return ret;
    }
    if ((size_t) n >= sizeof(line)) {
        n = sizeof(line) - 1;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    for (int i = 0; i < n; i++) {
        s_buf[s_head] = line[i];
        s_head = (s_head + 1) % LOG_BUFFER_CAPACITY;
        if (s_head == 0) {
            s_wrapped = true;
        }
    }
    xSemaphoreGive(s_mutex);

    if (s_sink) {
        xStreamBufferSend(s_sink, line, (size_t) n, 0);
    }

    return ret;
}

void log_buffer_init(void)
{
    s_mutex = xSemaphoreCreateMutex();
    s_orig_vprintf = esp_log_set_vprintf(log_buffer_vprintf);
}

size_t log_buffer_read(char *out, size_t max_len)
{
    if (!s_mutex || max_len == 0) {
        return 0;
    }

    xSemaphoreTake(s_mutex, portMAX_DELAY);
    size_t total = s_wrapped ? LOG_BUFFER_CAPACITY : s_head;
    size_t oldest = s_wrapped ? s_head : 0; // posizione del byte piu' vecchio disponibile
    // Se si chiede meno del disponibile, si vogliono i piu' RECENTI: si
    // salta la parte piu' vecchia invece di tagliare in fondo.
    size_t to_copy = total < max_len ? total : max_len;
    size_t skip = total - to_copy;
    size_t start = (oldest + skip) % LOG_BUFFER_CAPACITY;
    for (size_t i = 0; i < to_copy; i++) {
        out[i] = s_buf[(start + i) % LOG_BUFFER_CAPACITY];
    }
    xSemaphoreGive(s_mutex);

    return to_copy;
}
