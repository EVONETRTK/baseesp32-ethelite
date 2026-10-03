#include "remote_status.h"
#include "status.h"
#include "sys_stats.h"
#include "base_monitor.h"
#include "data_usage.h"
#include "sim_tools.h"
#include "time_sync.h"
#include "version.h"

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_log.h"
#include "esp_timer.h"

static const char *TAG = "remote_status";

static int64_t s_last_sent_us;
static char s_last_result[128];

const char *remote_status_last_result(void)
{
    return s_last_result;
}

// Riassunto dello stato: solo i dati utili a capire da lontano se la base
// lavora (niente impostazioni ne' password).
static char *build_json(const app_settings_t *cfg)
{
    cJSON *r = cJSON_CreateObject();
    if (!r) {
        return NULL;
    }
    char now[24];
    time_sync_format_now(now, sizeof(now));
    cJSON_AddStringToObject(r, "serial", cfg->device_serial);
    cJSON_AddStringToObject(r, "firmware", FIRMWARE_VERSION);
    cJSON_AddStringToObject(r, "time", now);
    cJSON_AddNumberToObject(r, "uptime_s", (double) (esp_timer_get_time() / 1000000));
    cJSON_AddStringToObject(r, "mode", status_get_active_rover() ? "rover" : "base");
    net_status_t net = status_get_net();
    cJSON_AddStringToObject(r, "net", net == NET_STATUS_WIFI ? "wifi" : (net == NET_STATUS_CELLULAR ? "cellulare" : "nessuna"));

    ntrip_conn_status_t nt = status_ntrip_get();
    cJSON_AddBoolToObject(r, "caster_connected", nt.connected);
    cJSON_AddStringToObject(r, "caster_last_error", nt.last_error);
    cJSON_AddStringToObject(r, "mountpoint", status_get_active_rover() ? cfg->rover_mountpoint : cfg->ntrip_mountpoint);

    base_monitor_status_t bm = base_monitor_get_status();
    cJSON_AddNumberToObject(r, "position_age_s", bm.last_position_us > 0
        ? (double) ((esp_timer_get_time() - bm.last_position_us) / 1000000) : -1.0);
    svin_status_t sv = status_svin_get();
    if (sv.have) {
        cJSON_AddBoolToObject(r, "svin_valid", sv.valid);
        cJSON_AddNumberToObject(r, "svin_acc_m", sv.mean_acc_m);
    }

    sys_stats_t st = sys_stats_get();
    cJSON_AddNumberToObject(r, "free_heap", st.free_heap_bytes);
    cJSON_AddNumberToObject(r, "min_free_heap", st.min_free_heap_bytes);
    if (st.chip_temp_c > -1000) {
        cJSON_AddNumberToObject(r, "temp_c", st.chip_temp_c);
        cJSON_AddNumberToObject(r, "temp_max_c", st.chip_temp_max_c);
    }
    cJSON_AddStringToObject(r, "last_reset", sys_stats_last_reset());
    gnss_sys_status_t gs = status_gnss_sys_get();
    if (gs.have) {
        cJSON_AddNumberToObject(r, "gnss_temp_c", gs.temp_c);
        cJSON_AddNumberToObject(r, "gnss_errors", gs.errors);
    }
    cJSON_AddStringToObject(r, "last_crash", sys_stats_last_crash());

    data_usage_t du = data_usage_get();
    cJSON_AddNumberToObject(r, "data_cell_period_bytes", (double) du.cell_month);
    cJSON_AddNumberToObject(r, "data_plan_mb", cfg->data_plan_mb);
    static sim_tools_status_t sim; // ~400 byte, fuori dallo stack
    sim_tools_get_status(&sim);
    if (sim.credit_eur_valid) {
        cJSON_AddNumberToObject(r, "sim_credit_eur", sim.credit_eur);
    }
    char *out = cJSON_PrintUnformatted(r);
    cJSON_Delete(r);
    return out;
}

void remote_status_tick(const app_settings_t *cfg)
{
    if (cfg->remote_interval_min == 0 || !cfg->remote_url[0] || status_get_net() == NET_STATUS_NONE) {
        return;
    }
    int64_t now = esp_timer_get_time();
    if (s_last_sent_us != 0 && now - s_last_sent_us < (int64_t) cfg->remote_interval_min * 60 * 1000000) {
        return;
    }
    s_last_sent_us = now;

    char *json = build_json(cfg);
    if (!json) {
        snprintf(s_last_result, sizeof(s_last_result), "memoria insufficiente per preparare i dati");
        return;
    }
    esp_http_client_config_t hc = {
        .url = cfg->remote_url,
        .method = HTTP_METHOD_POST,
        .timeout_ms = 10000,
        .crt_bundle_attach = esp_crt_bundle_attach,
    };
    esp_http_client_handle_t c = esp_http_client_init(&hc);
    if (!c) {
        free(json);
        snprintf(s_last_result, sizeof(s_last_result), "indirizzo non valido");
        return;
    }
    esp_http_client_set_header(c, "Content-Type", "application/json");
    esp_http_client_set_post_field(c, json, (int) strlen(json));
    esp_err_t err = esp_http_client_perform(c);
    int code = esp_http_client_get_status_code(c);
    esp_http_client_cleanup(c);
    free(json);

    char when[24];
    time_sync_format_now(when, sizeof(when));
    if (err == ESP_OK && code >= 200 && code < 300) {
        snprintf(s_last_result, sizeof(s_last_result), "inviato alle %s (HTTP %d)", when[0] ? when + 11 : "?", code);
    } else {
        snprintf(s_last_result, sizeof(s_last_result), "fallito alle %s: %s (HTTP %d)", when[0] ? when + 11 : "?",
                 err == ESP_OK ? "risposta del server" : esp_err_to_name(err), code);
        ESP_LOGW(TAG, "Invio dello stato a %s fallito: %s, HTTP %d", cfg->remote_url, esp_err_to_name(err), code);
    }
}
