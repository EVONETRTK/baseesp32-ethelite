#include "status.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

static volatile net_status_t s_net = NET_STATUS_NONE;
static volatile uint32_t s_rtcm_bytes = 0;
static volatile int64_t s_last_rtcm_us = 0;

static ntrip_conn_status_t s_ntrip;
static ntrip_outage_t s_outages[NTRIP_OUTAGE_LOG_LEN]; // anello, s_outage_n totali registrate
static uint32_t s_outage_n;
static SemaphoreHandle_t s_ntrip_mutex;

static void ntrip_mutex_init(void)
{
    if (!s_ntrip_mutex) {
        s_ntrip_mutex = xSemaphoreCreateMutex();
    }
}

void status_set_net(net_status_t s)
{
    s_net = s;
}

net_status_t status_get_net(void)
{
    return s_net;
}

void status_note_rtcm_bytes(uint32_t n)
{
    s_rtcm_bytes += n;
    s_last_rtcm_us = esp_timer_get_time();
}

uint32_t status_get_rtcm_total_bytes(void)
{
    return s_rtcm_bytes;
}

int64_t status_get_last_rtcm_time_us(void)
{
    return s_last_rtcm_us;
}

static volatile int64_t s_last_gga_sent_us = 0;

void status_note_gga_sent(void)
{
    s_last_gga_sent_us = esp_timer_get_time();
}

int64_t status_get_last_gga_sent_time_us(void)
{
    return s_last_gga_sent_us;
}

static volatile int64_t s_last_online_update_check_us = 0;

void status_note_online_update_checked(void)
{
    s_last_online_update_check_us = esp_timer_get_time();
}

int64_t status_get_last_online_update_check_us(void)
{
    return s_last_online_update_check_us;
}

void status_ntrip_note_connected(void)
{
    ntrip_mutex_init();
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    if (s_outage_n > 0) {
        ntrip_outage_t *o = &s_outages[(s_outage_n - 1) % NTRIP_OUTAGE_LOG_LEN];
        if (o->end_us == 0) {
            o->end_us = esp_timer_get_time();
        }
    }
    s_ntrip.connected = true;
    s_ntrip.connected_since_us = esp_timer_get_time();
    s_ntrip.connect_count++;
    s_ntrip.last_error[0] = '\0';
    xSemaphoreGive(s_ntrip_mutex);
}

void status_ntrip_note_disconnected(const char *reason)
{
    ntrip_mutex_init();
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    if (s_ntrip.connected) {
        // Inizio di una caduta (i tentativi falliti successivi la allungano).
        ntrip_outage_t *o = &s_outages[s_outage_n % NTRIP_OUTAGE_LOG_LEN];
        o->start_us = esp_timer_get_time();
        o->end_us = 0;
        strncpy(o->reason, reason ? reason : "", sizeof(o->reason) - 1);
        o->reason[sizeof(o->reason) - 1] = '\0';
        s_outage_n++;
    }
    s_ntrip.connected = false;
    s_ntrip.connected_since_us = 0;
    s_ntrip.last_disconnect_us = esp_timer_get_time();
    if (reason) {
        strncpy(s_ntrip.last_error, reason, sizeof(s_ntrip.last_error) - 1);
        s_ntrip.last_error[sizeof(s_ntrip.last_error) - 1] = '\0';
    }
    xSemaphoreGive(s_ntrip_mutex);
}

int status_ntrip_get_outages(ntrip_outage_t *out, int max)
{
    if (!s_ntrip_mutex) {
        return 0;
    }
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    int n = 0;
    for (uint32_t i = s_outage_n; i > 0 && n < max && s_outage_n - i < NTRIP_OUTAGE_LOG_LEN; i--) {
        out[n++] = s_outages[(i - 1) % NTRIP_OUTAGE_LOG_LEN];
    }
    xSemaphoreGive(s_ntrip_mutex);
    return n;
}

ntrip_conn_status_t status_ntrip_get(void)
{
    if (!s_ntrip_mutex) {
        return (ntrip_conn_status_t){0};
    }
    ntrip_conn_status_t copy;
    xSemaphoreTake(s_ntrip_mutex, portMAX_DELAY);
    copy = s_ntrip;
    xSemaphoreGive(s_ntrip_mutex);
    return copy;
}

static bynav_ins_status_t s_bynav_ins;
static SemaphoreHandle_t s_bynav_ins_mutex;

static void bynav_ins_mutex_init(void)
{
    if (!s_bynav_ins_mutex) {
        s_bynav_ins_mutex = xSemaphoreCreateMutex();
    }
}

void status_bynav_ins_note_inspvaxa(const char *ins_status, float roll_deg, float pitch_deg, float heading_deg)
{
    bynav_ins_mutex_init();
    xSemaphoreTake(s_bynav_ins_mutex, portMAX_DELAY);
    s_bynav_ins.have_attitude = true;
    if (ins_status) {
        strncpy(s_bynav_ins.ins_status, ins_status, sizeof(s_bynav_ins.ins_status) - 1);
        s_bynav_ins.ins_status[sizeof(s_bynav_ins.ins_status) - 1] = '\0';
    }
    s_bynav_ins.roll_deg = roll_deg;
    s_bynav_ins.pitch_deg = pitch_deg;
    s_bynav_ins.heading_deg = heading_deg;
    s_bynav_ins.last_update_us = esp_timer_get_time();
    xSemaphoreGive(s_bynav_ins_mutex);
}

void status_bynav_ins_note_headinga(float heading_deg, float pitch_deg)
{
    bynav_ins_mutex_init();
    xSemaphoreTake(s_bynav_ins_mutex, portMAX_DELAY);
    s_bynav_ins.have_attitude = true;
    s_bynav_ins.heading_deg = heading_deg;
    s_bynav_ins.pitch_deg = pitch_deg;
    s_bynav_ins.last_update_us = esp_timer_get_time();
    xSemaphoreGive(s_bynav_ins_mutex);
}

bynav_ins_status_t status_bynav_ins_get(void)
{
    if (!s_bynav_ins_mutex) {
        return (bynav_ins_status_t){0};
    }
    bynav_ins_status_t copy;
    xSemaphoreTake(s_bynav_ins_mutex, portMAX_DELAY);
    copy = s_bynav_ins;
    xSemaphoreGive(s_bynav_ins_mutex);
    return copy;
}

static bool s_active_rover;

void status_set_active_rover(bool rover)
{
    s_active_rover = rover;
}

bool status_get_active_rover(void)
{
    return s_active_rover;
}

// Scritto dal task che legge il ricevitore, letto dal server web: struct
// piccola, copiata sotto sezione critica.
static svin_status_t s_svin;
static portMUX_TYPE s_svin_lock = portMUX_INITIALIZER_UNLOCKED;

void status_svin_note(bool active, bool valid, uint32_t duration_s, float mean_acc_m, uint32_t observations)
{
    portENTER_CRITICAL(&s_svin_lock);
    s_svin.have = true;
    s_svin.active = active;
    s_svin.valid = valid;
    s_svin.duration_s = duration_s;
    s_svin.mean_acc_m = mean_acc_m;
    s_svin.observations = observations;
    s_svin.last_update_us = esp_timer_get_time();
    portEXIT_CRITICAL(&s_svin_lock);
}

svin_status_t status_svin_get(void)
{
    portENTER_CRITICAL(&s_svin_lock);
    svin_status_t copy = s_svin;
    portEXIT_CRITICAL(&s_svin_lock);
    return copy;
}
