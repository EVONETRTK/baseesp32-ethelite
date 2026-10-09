// Sostituti sul PC delle funzioni di ESP-IDF e FreeRTOS usate dai moduli
// provati (vedi gli header in questa cartella). Un solo thread: nessun task
// parte davvero, mutex e sezioni critiche non fanno nulla.
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_err.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_system.h"
#include "esp_app_desc.h"
#include "nvs.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "freertos/idf_additions.h"
#include "freertos/message_buffer.h"

// --- log -------------------------------------------------------------------
// Messaggi del firmware stampati solo con PROVE_LOG=1.
void host_log(char level, const char *tag, const char *fmt, ...)
{
    static int enabled = -1;
    if (enabled < 0) {
        const char *e = getenv("PROVE_LOG");
        enabled = e && e[0] == '1';
    }
    if (!enabled) {
        return;
    }
    va_list ap;
    va_start(ap, fmt);
    printf("        [%c %s] ", level, tag);
    vprintf(fmt, ap);
    printf("\n");
    va_end(ap);
}

const char *esp_err_to_name(esp_err_t code)
{
    return code == ESP_OK ? "ESP_OK" : "ESP_ERR";
}

// --- tempo -----------------------------------------------------------------
// Ogni lettura avanza di 1 ms, cosi' due eventi di fila hanno tempi diversi;
// le prove possono fissarlo o farlo avanzare.
static int64_t s_now_us = 1000000;

int64_t esp_timer_get_time(void)
{
    s_now_us += 1000;
    return s_now_us;
}

void host_time_set_us(int64_t us)
{
    s_now_us = us - 1000;
}

void host_time_advance_us(int64_t us)
{
    s_now_us += us;
}

// --- sistema ---------------------------------------------------------------
int host_restart_count;

void esp_restart(void)
{
    host_restart_count++;
}

const esp_app_desc_t *esp_app_get_description(void)
{
    static const esp_app_desc_t d = { .version = "prova", .project_name = "prove", .time = "12:00:00", .date = "Oct  9 2026" };
    return &d;
}

// --- NVS: assente sul PC ----------------------------------------------------
esp_err_t nvs_open(const char *ns, nvs_open_mode_t mode, nvs_handle_t *h) { (void) ns; (void) mode; (void) h; return ESP_FAIL; }
void nvs_close(nvs_handle_t h) { (void) h; }
esp_err_t nvs_commit(nvs_handle_t h) { (void) h; return ESP_FAIL; }
esp_err_t nvs_get_blob(nvs_handle_t h, const char *k, void *o, size_t *l) { (void) h; (void) k; (void) o; (void) l; return ESP_FAIL; }
esp_err_t nvs_set_blob(nvs_handle_t h, const char *k, const void *v, size_t l) { (void) h; (void) k; (void) v; (void) l; return ESP_FAIL; }
esp_err_t nvs_erase_key(nvs_handle_t h, const char *k) { (void) h; (void) k; return ESP_FAIL; }

// --- semafori: sempre liberi ------------------------------------------------
struct host_sem {
    int unused;
};

SemaphoreHandle_t xSemaphoreCreateMutex(void)
{
    return calloc(1, sizeof(struct host_sem));
}

BaseType_t xSemaphoreTake(SemaphoreHandle_t s, TickType_t t) { (void) s; (void) t; return pdTRUE; }
BaseType_t xSemaphoreGive(SemaphoreHandle_t s) { (void) s; return pdTRUE; }

// --- code ------------------------------------------------------------------
struct host_queue {
    UBaseType_t len, size, count, head;
    uint8_t *data;
};

QueueHandle_t xQueueCreate(UBaseType_t len, UBaseType_t item_size)
{
    struct host_queue *q = calloc(1, sizeof(*q));
    q->len = len;
    q->size = item_size;
    q->data = calloc(len, item_size);
    return q;
}

BaseType_t xQueueReset(QueueHandle_t q)
{
    q->count = q->head = 0;
    return pdPASS;
}

BaseType_t xQueueSend(QueueHandle_t q, const void *item, TickType_t t)
{
    (void) t;
    if (q->count >= q->len) {
        return pdFALSE;
    }
    memcpy(q->data + ((q->head + q->count) % q->len) * q->size, item, q->size);
    q->count++;
    return pdTRUE;
}

BaseType_t xQueueOverwrite(QueueHandle_t q, const void *item)
{
    // Come FreeRTOS: solo per code da un elemento.
    memcpy(q->data, item, q->size);
    q->head = 0;
    q->count = 1;
    return pdPASS;
}

BaseType_t xQueueReceive(QueueHandle_t q, void *out, TickType_t t)
{
    (void) t;
    if (q->count == 0) {
        return pdFALSE;
    }
    memcpy(out, q->data + q->head * q->size, q->size);
    q->head = (q->head + 1) % q->len;
    q->count--;
    return pdTRUE;
}

// --- task: non partono -------------------------------------------------------
int host_task_created;

void vTaskDelay(TickType_t t)
{
    host_time_advance_us((int64_t) t * 1000);
}

BaseType_t xTaskCreate(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio, TaskHandle_t *h)
{
    (void) fn; (void) name; (void) stack; (void) arg; (void) prio;
    if (h) {
        *h = NULL;
    }
    host_task_created++;
    return pdPASS;
}

BaseType_t xTaskCreatePinnedToCore(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                                   TaskHandle_t *h, BaseType_t core)
{
    (void) core;
    return xTaskCreate(fn, name, stack, arg, prio, h);
}

BaseType_t xTaskCreateWithCaps(TaskFunction_t fn, const char *name, uint32_t stack, void *arg, UBaseType_t prio,
                               TaskHandle_t *h, uint32_t caps)
{
    (void) caps;
    return xTaskCreate(fn, name, stack, arg, prio, h);
}

UBaseType_t uxTaskGetStackHighWaterMark(TaskHandle_t h)
{
    (void) h;
    return 4096;
}

// --- message buffer: messaggi in fila in un buffer unico -----------------------
struct host_msgbuf {
    size_t cap, used;
    uint8_t *data; // [lunghezza (size_t)][byte] ripetuti
};

MessageBufferHandle_t xMessageBufferCreate(size_t size)
{
    struct host_msgbuf *b = calloc(1, sizeof(*b));
    b->cap = size;
    b->data = malloc(size);
    return b;
}

MessageBufferHandle_t xMessageBufferCreateStatic(size_t size, uint8_t *storage, StaticMessageBuffer_t *st)
{
    (void) st;
    struct host_msgbuf *b = calloc(1, sizeof(*b));
    b->cap = size;
    b->data = storage;
    return b;
}

size_t xMessageBufferSend(MessageBufferHandle_t b, const void *data, size_t len, TickType_t t)
{
    (void) t;
    if (b->used + sizeof(size_t) + len > b->cap) {
        return 0;
    }
    memcpy(b->data + b->used, &len, sizeof(size_t));
    memcpy(b->data + b->used + sizeof(size_t), data, len);
    b->used += sizeof(size_t) + len;
    return len;
}

size_t xMessageBufferReceive(MessageBufferHandle_t b, void *out, size_t cap, TickType_t t)
{
    (void) t;
    if (b->used == 0) {
        return 0;
    }
    size_t len;
    memcpy(&len, b->data, sizeof(size_t));
    if (len > cap) {
        return 0;
    }
    memcpy(out, b->data + sizeof(size_t), len);
    memmove(b->data, b->data + sizeof(size_t) + len, b->used - sizeof(size_t) - len);
    b->used -= sizeof(size_t) + len;
    return len;
}
