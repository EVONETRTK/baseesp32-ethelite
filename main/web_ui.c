#include "web_ui.h"
#include "version.h"
#include "settings.h"
#include "ota_update.h"
#include "sd_update.h"
#include "online_update.h"
#include "status.h"
#include "log_buffer.h"
#include "sys_stats.h"
#include "gnss_signal.h"
#include "gnss_fix.h"
#include "wifi_link.h"
#include "cellular_link.h"
#include "alerts.h"
#include "base_monitor.h"
#include "ntrip_caster_server.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"

static const char *TAG = "web_ui";

extern const uint8_t index_html_start[] asm("_binary_index_html_start");
extern const uint8_t index_html_end[]   asm("_binary_index_html_end");

// HTTP Basic Auth, utente fisso "admin" + codice impostabile dalla UI
// stessa (settings.admin_code). Un codice vuoto disabilita la protezione
// (usato anche come stato transitorio, es. subito dopo un flash pulito
// prima che venga letta la configurazione - non dovrebbe capitare visto
// che apply_defaults() imposta sempre un codice di default).
static bool check_auth(httpd_req_t *req)
{
    app_settings_t s = settings_get();
    if (strlen(s.admin_code) == 0) {
        return true;
    }

    char auth_hdr[128];
    if (httpd_req_get_hdr_value_str(req, "Authorization", auth_hdr, sizeof(auth_hdr)) != ESP_OK) {
        return false;
    }
    if (strncmp(auth_hdr, "Basic ", 6) != 0) {
        return false;
    }

    unsigned char decoded[96];
    size_t decoded_len = 0;
    if (mbedtls_base64_decode(decoded, sizeof(decoded) - 1, &decoded_len,
                               (const unsigned char *) auth_hdr + 6, strlen(auth_hdr + 6)) != 0) {
        return false;
    }
    decoded[decoded_len] = '\0';

    char expected[128];
    snprintf(expected, sizeof(expected), "admin:%s", s.admin_code);

    return strcmp((const char *) decoded, expected) == 0;
}

// Cookie di sessione: il popup Basic Auth del browser non viene ricordato
// in modo affidabile su tutti i browser/situazioni (es. passando tra
// l'IP dell'AP 192.168.4.1 e quello della rete di casa, il browser li
// tratta come siti diversi e chiede di nuovo le credenziali). Dopo un
// primo accesso riuscito via Basic Auth, impostiamo un cookie che vale
// da solo per ~30 giorni sullo stesso indirizzo, senza dover reinserire
// nulla.
#define AUTH_COOKIE_NAME "evonetrtk_auth"
#define AUTH_COOKIE_MAX_AGE_S (30 * 24 * 3600)

static bool check_auth_cookie(httpd_req_t *req, const app_settings_t *s)
{
    if (strlen(s->admin_code) == 0) {
        return true;
    }
    char cookie_hdr[160];
    if (httpd_req_get_hdr_value_str(req, "Cookie", cookie_hdr, sizeof(cookie_hdr)) != ESP_OK) {
        return false;
    }
    char expected[160];
    snprintf(expected, sizeof(expected), AUTH_COOKIE_NAME "=%s", s->admin_code);
    return strstr(cookie_hdr, expected) != NULL;
}

static esp_err_t require_auth(httpd_req_t *req)
{
    // Copre in un colpo solo tutte le richieste protette (ogni handler
    // eccetto index_get_handler chiama questa funzione per prima cosa):
    // conferma se una richiesta arriva davvero al dispositivo, cosa non
    // ovvia dal solo log applicativo dato che molti handler non
    // scrivono nulla quando vanno a buon fine.
    ESP_LOGI(TAG, "Richiesta %s %s", http_method_str(req->method), req->uri);

    app_settings_t s = settings_get();

    if (check_auth_cookie(req, &s)) {
        return ESP_OK;
    }

    if (check_auth(req)) {
        // Autenticato via Basic Auth: imposta anche il cookie di sessione
        // cosi' le richieste successive non lo richiedono piu'. Buffer
        // "static": il server web qui gestisce una richiesta alla volta
        // (nessun worker parallelo configurato), il valore resta valido
        // fino a quando httpd_resp_send* viene chiamato piu' avanti nello
        // stesso handler che ha invocato questa funzione.
        static char cookie_val[192];
        snprintf(cookie_val, sizeof(cookie_val), AUTH_COOKIE_NAME "=%s; Max-Age=%d; Path=/",
                 s.admin_code, AUTH_COOKIE_MAX_AGE_S);
        httpd_resp_set_hdr(req, "Set-Cookie", cookie_val);
        return ESP_OK;
    }

    ESP_LOGW(TAG, "Autenticazione fallita per %s", req->uri);
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"EVONETRTK\"");
    httpd_resp_send(req, NULL, 0);
    return ESP_FAIL;
}

static esp_err_t index_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_set_type(req, "text/html; charset=utf-8");
    size_t len = index_html_end - index_html_start;
    return httpd_resp_send(req, (const char *) index_html_start, len);
}

static const char *net_status_str(net_status_t s)
{
    switch (s) {
    case NET_STATUS_WIFI:     return "wifi";
    case NET_STATUS_CELLULAR: return "cellulare";
    default:                  return "nessuna";
    }
}

static const char *gnss_chip_str(gnss_chip_t c)
{
    switch (c) {
    case GNSS_CHIP_UNICORE: return "unicore";
    case GNSS_CHIP_LC29H:   return "lc29h";
    default:                return "ublox";
    }
}

static const char *device_mode_str(device_mode_t m)
{
    return m == DEVICE_MODE_ROVER ? "rover" : "base";
}

static const char *oled_controller_str(oled_controller_t c)
{
    switch (c) {
    case OLED_CTRL_SH1106:  return "sh1106";
    case OLED_CTRL_SSD1309: return "ssd1309";
    default:                return "ssd1306";
    }
}

static const char *network_mode_str(network_mode_t m)
{
    switch (m) {
    case NETWORK_MODE_WIFI_ONLY:     return "wifi";
    case NETWORK_MODE_CELLULAR_ONLY: return "cellular";
    default:                         return "both";
    }
}

static const char *rgb_mode_str(rgb_led_mode_t m)
{
    switch (m) {
    case RGB_LED_WS2812: return "ws2812";
    case RGB_LED_PWM3:   return "pwm3";
    default:              return "none";
    }
}

static esp_err_t status_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    app_settings_t s = settings_get();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddStringToObject(root, "net", net_status_str(status_get_net()));
    cJSON_AddNumberToObject(root, "rtcm_bytes", status_get_rtcm_total_bytes());
    cJSON_AddNumberToObject(root, "last_rtcm_us", (double) status_get_last_rtcm_time_us());

    ntrip_conn_status_t ntrip = status_ntrip_get();
    cJSON_AddBoolToObject(root, "ntrip_connected", ntrip.connected);
    cJSON_AddNumberToObject(root, "ntrip_connected_since_us", (double) ntrip.connected_since_us);
    cJSON_AddNumberToObject(root, "ntrip_last_disconnect_us", (double) ntrip.last_disconnect_us);
    cJSON_AddNumberToObject(root, "ntrip_connect_count", ntrip.connect_count);
    cJSON_AddStringToObject(root, "ntrip_last_error", ntrip.last_error);
    // Tempo del dispositivo (dal boot, stessa base di ntrip_connected_since_us
    // e last_disconnect_us) al momento di generare questa risposta - permette
    // al browser di calcolare "da quanto" senza affidarsi al proprio
    // orologio (che non ha comunque relazione con l'uptime del dispositivo).
    cJSON_AddNumberToObject(root, "now_us", (double) esp_timer_get_time());
    cJSON_AddStringToObject(root, "gnss_chip", gnss_chip_str(s.gnss_chip));
    cJSON_AddStringToObject(root, "device_mode", device_mode_str(s.device_mode));
    cJSON_AddStringToObject(root, "network_mode", network_mode_str(s.network_mode));
    cJSON_AddStringToObject(root, "wifi_ssid", s.wifi_ssid);
    cJSON_AddStringToObject(root, "cellular_apn", s.cellular_apn);
    cJSON_AddBoolToObject(root, "cellular_is_sim868", s.cellular_is_sim868);
    cJSON_AddStringToObject(root, "ntrip_host", s.ntrip_host);
    cJSON_AddNumberToObject(root, "ntrip_port", s.ntrip_port);
    cJSON_AddStringToObject(root, "ntrip_mountpoint", s.ntrip_mountpoint);
    cJSON_AddStringToObject(root, "ntrip_username", s.ntrip_username);
    cJSON_AddStringToObject(root, "ap_ssid", s.ap_ssid);
    cJSON_AddStringToObject(root, "device_serial", s.device_serial);
    cJSON_AddStringToObject(root, "firmware_version", FIRMWARE_VERSION);
    cJSON_AddStringToObject(root, "ota_update_url", s.ota_update_url);
    cJSON_AddNumberToObject(root, "nmea_udp_port", s.nmea_udp_port);

    cJSON_AddBoolToObject(root, "alert_enable", s.alert_enable);
    cJSON_AddNumberToObject(root, "alert_threshold_min", s.alert_threshold_min);
    cJSON_AddStringToObject(root, "alert_smtp_host", s.alert_smtp_host);
    cJSON_AddNumberToObject(root, "alert_smtp_port", s.alert_smtp_port);
    cJSON_AddStringToObject(root, "alert_smtp_user", s.alert_smtp_user);
    cJSON_AddStringToObject(root, "alert_email_to", s.alert_email_to);
    cJSON_AddStringToObject(root, "alert_whatsapp_phone", s.alert_whatsapp_phone);
    // alert_smtp_password e alert_whatsapp_apikey non vengono mai
    // restituiti (come ntrip_password sopra) - solo scrivibili dalla UI,
    // mai riletti.

    cJSON_AddBoolToObject(root, "base_drift_alert_enable", s.base_drift_alert_enable);
    cJSON_AddNumberToObject(root, "base_drift_threshold_m", s.base_drift_threshold_m);
    base_monitor_status_t drift = base_monitor_get_status();
    cJSON_AddBoolToObject(root, "base_drift_baseline_set", drift.baseline_set);
    if (drift.baseline_set) {
        cJSON_AddNumberToObject(root, "base_drift_m", drift.drift_m);
    }

    cJSON_AddBoolToObject(root, "ntrip_caster_server_enable", s.ntrip_caster_server_enable);
    cJSON_AddNumberToObject(root, "ntrip_caster_server_port", s.ntrip_caster_server_port);
    cJSON_AddStringToObject(root, "ntrip_caster_server_mountpoint", s.ntrip_caster_server_mountpoint);
    cJSON_AddStringToObject(root, "ntrip_caster_server_username", s.ntrip_caster_server_username);
    cJSON_AddNumberToObject(root, "ntrip_caster_server_clients", (double) ntrip_caster_server_get_client_count());
    // ntrip_caster_server_password non viene mai restituita (come le altre
    // password sopra) - solo scrivibile dalla UI, mai riletta.

    sd_update_status_t sd_status = sd_update_get_status();
    cJSON_AddBoolToObject(root, "sd_checked", sd_status.checked);
    if (sd_status.checked) {
        cJSON_AddBoolToObject(root, "sd_card_present", sd_status.card_present);
        cJSON_AddStringToObject(root, "sd_message", sd_status.message);
        if (sd_status.card_present) {
            cJSON_AddNumberToObject(root, "sd_total_bytes", (double) sd_status.total_bytes);
            cJSON_AddNumberToObject(root, "sd_used_bytes", (double) sd_status.used_bytes);
        }
    }

    // Bluetooth Classic (SPP) non disponibile su ESP32-S3 (solo BLE, non
    // implementata su questa scheda) - i campi bt_* non vengono inviati.

    cJSON_AddNumberToObject(root, "gnss_uart_num", s.gnss_uart_num);
    cJSON_AddNumberToObject(root, "gnss_uart_tx_pin", s.gnss_uart_tx_pin);
    cJSON_AddNumberToObject(root, "gnss_uart_rx_pin", s.gnss_uart_rx_pin);
    cJSON_AddNumberToObject(root, "gnss_uart_baud", s.gnss_uart_baud);

    cJSON_AddStringToObject(root, "rgb_led_mode", rgb_mode_str(s.rgb_led_mode));
    cJSON_AddNumberToObject(root, "rgb_led_ws2812_pin", s.rgb_led_ws2812_pin);
    cJSON_AddNumberToObject(root, "rgb_led_pwm_r_pin", s.rgb_led_pwm_r_pin);
    cJSON_AddNumberToObject(root, "rgb_led_pwm_g_pin", s.rgb_led_pwm_g_pin);
    cJSON_AddNumberToObject(root, "rgb_led_pwm_b_pin", s.rgb_led_pwm_b_pin);
    cJSON_AddBoolToObject(root, "rgb_led_pwm_active_low", s.rgb_led_pwm_active_low);

    cJSON_AddNumberToObject(root, "oled_sda_pin", s.oled_sda_pin);
    cJSON_AddNumberToObject(root, "oled_scl_pin", s.oled_scl_pin);
    cJSON_AddNumberToObject(root, "oled_i2c_addr", s.oled_i2c_addr);
    cJSON_AddStringToObject(root, "oled_controller", oled_controller_str(s.oled_controller));
    cJSON_AddBoolToObject(root, "oled_flip_h", s.oled_flip_h);
    cJSON_AddBoolToObject(root, "oled_flip_v", s.oled_flip_v);

    sys_stats_t stats = sys_stats_get();
    cJSON_AddNumberToObject(root, "free_heap_bytes", stats.free_heap_bytes);
    cJSON_AddNumberToObject(root, "min_free_heap_bytes", stats.min_free_heap_bytes);
    cJSON_AddNumberToObject(root, "total_heap_bytes", stats.total_heap_bytes);
    cJSON_AddBoolToObject(root, "psram_present", stats.psram_present);
    if (stats.psram_present) {
        cJSON_AddNumberToObject(root, "free_psram_bytes", stats.free_psram_bytes);
        cJSON_AddNumberToObject(root, "total_psram_bytes", stats.total_psram_bytes);
    }
    if (stats.cpu0_percent >= 0) {
        cJSON_AddNumberToObject(root, "cpu0_percent", stats.cpu0_percent);
    }
    if (stats.cpu1_percent >= 0) {
        cJSON_AddNumberToObject(root, "cpu1_percent", stats.cpu1_percent);
    }
    if (stats.chip_temp_c > -1000) {
        cJSON_AddNumberToObject(root, "chip_temp_c", stats.chip_temp_c);
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t signals_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    cJSON *root = cJSON_CreateObject();

    cJSON *sats = cJSON_CreateArray();
    gnss_sat_signal_t sat_list[GNSS_SIGNAL_MAX_SATS];
    size_t n = gnss_signal_get_satellites(sat_list, GNSS_SIGNAL_MAX_SATS);
    for (size_t i = 0; i < n; i++) {
        cJSON *sat = cJSON_CreateObject();
        cJSON_AddStringToObject(sat, "constellation", sat_list[i].constellation);
        cJSON_AddNumberToObject(sat, "prn", sat_list[i].prn);
        cJSON_AddNumberToObject(sat, "snr", sat_list[i].snr);
        cJSON_AddItemToArray(sats, sat);
    }
    cJSON_AddItemToObject(root, "satellites", sats);

    gnss_fix_status_t fix = gnss_fix_get_status();
    cJSON_AddBoolToObject(root, "gnss_fix_valid", fix.valid);
    if (fix.valid) {
        cJSON_AddNumberToObject(root, "gnss_fix_quality", fix.quality);
        cJSON_AddStringToObject(root, "gnss_fix_quality_str", gnss_fix_quality_str(fix.quality));
        cJSON_AddNumberToObject(root, "gnss_satellites_used", fix.satellites_used);
        cJSON_AddNumberToObject(root, "gnss_hdop", fix.hdop);
        cJSON_AddNumberToObject(root, "gnss_altitude_m", fix.altitude_m);
        cJSON_AddNumberToObject(root, "gnss_diff_age_s", fix.diff_age_s);
        double fix_age_s = (esp_timer_get_time() - fix.last_update_us) / 1e6;
        cJSON_AddNumberToObject(root, "gnss_fix_age_s", fix_age_s);
    }

    int8_t wifi_rssi;
    if (wifi_link_get_rssi(&wifi_rssi)) {
        cJSON_AddNumberToObject(root, "wifi_rssi_dbm", wifi_rssi);
    } else {
        cJSON_AddNullToObject(root, "wifi_rssi_dbm");
    }

    int cell_rssi;
    if (cellular_link_get_signal(&cell_rssi)) {
        cJSON_AddNumberToObject(root, "cellular_rssi_dbm", cell_rssi);
    } else {
        cJSON_AddNullToObject(root, "cellular_rssi_dbm");
    }

    char cell_operator[32] = {0};
    char cell_tech[16] = {0};
    if (cellular_link_get_operator_info(cell_operator, sizeof(cell_operator), cell_tech, sizeof(cell_tech))) {
        cJSON_AddStringToObject(root, "cellular_operator", cell_operator);
        cJSON_AddStringToObject(root, "cellular_tech", cell_tech);
    } else {
        cJSON_AddNullToObject(root, "cellular_operator");
        cJSON_AddNullToObject(root, "cellular_tech");
    }

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

// Copia il campo stringa solo se presente e non vuoto: una stringa vuota
// nel form significa "lascia invariato", non "cancella" - evita di
// azzerare per sbaglio una password gia' salvata quando l'utente
// aggiorna solo un altro campo.
static void copy_field(const cJSON *root, const char *key, char *dst, size_t dst_size)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item && cJSON_IsString(item) && item->valuestring && item->valuestring[0] != '\0') {
        strncpy(dst, item->valuestring, dst_size - 1);
        dst[dst_size - 1] = '\0';
    }
}

// Campo numerico di pin/GPIO: -1 = non usato, valido anche come "assente"
// esplicito dal form (a differenza delle stringhe, qui il valore va
// sempre applicato se presente, incluso -1).
static void copy_pin_field(const cJSON *root, const char *key, int *dst)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (item && cJSON_IsNumber(item) && item->valueint >= -1 && item->valueint < 256) {
        *dst = item->valueint;
    }
}

static esp_err_t settings_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 2048) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char *buf = malloc(req->content_len + 1);
    if (!buf) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria esaurita");
        return ESP_FAIL;
    }

    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            free(buf);
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    free(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }

    app_settings_t s = settings_get();

    copy_field(root, "wifi_ssid", s.wifi_ssid, sizeof(s.wifi_ssid));
    copy_field(root, "wifi_password", s.wifi_password, sizeof(s.wifi_password));
    copy_field(root, "cellular_apn", s.cellular_apn, sizeof(s.cellular_apn));
    copy_field(root, "ntrip_host", s.ntrip_host, sizeof(s.ntrip_host));
    copy_field(root, "ntrip_mountpoint", s.ntrip_mountpoint, sizeof(s.ntrip_mountpoint));
    copy_field(root, "ntrip_username", s.ntrip_username, sizeof(s.ntrip_username));
    copy_field(root, "ntrip_password", s.ntrip_password, sizeof(s.ntrip_password));
    copy_field(root, "ap_ssid", s.ap_ssid, sizeof(s.ap_ssid));
    copy_field(root, "ap_password", s.ap_password, sizeof(s.ap_password));
    copy_field(root, "admin_code", s.admin_code, sizeof(s.admin_code));
    copy_field(root, "device_serial", s.device_serial, sizeof(s.device_serial));
    copy_field(root, "ota_update_url", s.ota_update_url, sizeof(s.ota_update_url));
    copy_field(root, "alert_smtp_host", s.alert_smtp_host, sizeof(s.alert_smtp_host));
    copy_field(root, "alert_smtp_user", s.alert_smtp_user, sizeof(s.alert_smtp_user));
    copy_field(root, "alert_smtp_password", s.alert_smtp_password, sizeof(s.alert_smtp_password));
    copy_field(root, "alert_email_to", s.alert_email_to, sizeof(s.alert_email_to));
    copy_field(root, "alert_whatsapp_phone", s.alert_whatsapp_phone, sizeof(s.alert_whatsapp_phone));
    copy_field(root, "alert_whatsapp_apikey", s.alert_whatsapp_apikey, sizeof(s.alert_whatsapp_apikey));
    copy_field(root, "ntrip_caster_server_mountpoint", s.ntrip_caster_server_mountpoint, sizeof(s.ntrip_caster_server_mountpoint));
    copy_field(root, "ntrip_caster_server_username", s.ntrip_caster_server_username, sizeof(s.ntrip_caster_server_username));
    copy_field(root, "ntrip_caster_server_password", s.ntrip_caster_server_password, sizeof(s.ntrip_caster_server_password));

    cJSON *alert_enable_item = cJSON_GetObjectItemCaseSensitive(root, "alert_enable");
    if (alert_enable_item && cJSON_IsBool(alert_enable_item)) {
        s.alert_enable = cJSON_IsTrue(alert_enable_item);
    }
    cJSON *alert_threshold_item = cJSON_GetObjectItemCaseSensitive(root, "alert_threshold_min");
    if (alert_threshold_item && cJSON_IsNumber(alert_threshold_item) && alert_threshold_item->valueint > 0) {
        s.alert_threshold_min = (uint16_t) alert_threshold_item->valueint;
    }
    cJSON *alert_smtp_port_item = cJSON_GetObjectItemCaseSensitive(root, "alert_smtp_port");
    if (alert_smtp_port_item && cJSON_IsNumber(alert_smtp_port_item) &&
        alert_smtp_port_item->valueint > 0 && alert_smtp_port_item->valueint <= 65535) {
        s.alert_smtp_port = (uint16_t) alert_smtp_port_item->valueint;
    }
    cJSON *drift_enable_item = cJSON_GetObjectItemCaseSensitive(root, "base_drift_alert_enable");
    if (drift_enable_item && cJSON_IsBool(drift_enable_item)) {
        s.base_drift_alert_enable = cJSON_IsTrue(drift_enable_item);
    }
    cJSON *drift_threshold_item = cJSON_GetObjectItemCaseSensitive(root, "base_drift_threshold_m");
    if (drift_threshold_item && cJSON_IsNumber(drift_threshold_item) && drift_threshold_item->valuedouble > 0) {
        s.base_drift_threshold_m = (float) drift_threshold_item->valuedouble;
    }
    cJSON *caster_srv_enable_item = cJSON_GetObjectItemCaseSensitive(root, "ntrip_caster_server_enable");
    if (caster_srv_enable_item && cJSON_IsBool(caster_srv_enable_item)) {
        s.ntrip_caster_server_enable = cJSON_IsTrue(caster_srv_enable_item);
    }
    cJSON *caster_srv_port_item = cJSON_GetObjectItemCaseSensitive(root, "ntrip_caster_server_port");
    if (caster_srv_port_item && cJSON_IsNumber(caster_srv_port_item) &&
        caster_srv_port_item->valueint > 0 && caster_srv_port_item->valueint <= 65535) {
        s.ntrip_caster_server_port = (uint16_t) caster_srv_port_item->valueint;
    }

    cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "ntrip_port");
    if (port_item && cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535) {
        s.ntrip_port = (uint16_t) port_item->valueint;
    }

    cJSON *udp_port_item = cJSON_GetObjectItemCaseSensitive(root, "nmea_udp_port");
    if (udp_port_item && cJSON_IsNumber(udp_port_item) && udp_port_item->valueint > 0 && udp_port_item->valueint <= 65535) {
        s.nmea_udp_port = (uint16_t) udp_port_item->valueint;
    }

    cJSON *chip_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_chip");
    if (chip_item && cJSON_IsString(chip_item)) {
        if (strcmp(chip_item->valuestring, "unicore") == 0) {
            s.gnss_chip = GNSS_CHIP_UNICORE;
        } else if (strcmp(chip_item->valuestring, "lc29h") == 0) {
            s.gnss_chip = GNSS_CHIP_LC29H;
        } else {
            s.gnss_chip = GNSS_CHIP_UBLOX;
        }
    }

    cJSON *mode_item = cJSON_GetObjectItemCaseSensitive(root, "device_mode");
    if (mode_item && cJSON_IsString(mode_item)) {
        s.device_mode = (strcmp(mode_item->valuestring, "rover") == 0) ? DEVICE_MODE_ROVER : DEVICE_MODE_BASE;
    }

    cJSON *cellular_modem_item = cJSON_GetObjectItemCaseSensitive(root, "cellular_is_sim868");
    if (cellular_modem_item && cJSON_IsBool(cellular_modem_item)) {
        s.cellular_is_sim868 = cJSON_IsTrue(cellular_modem_item);
    }

    cJSON *net_mode_item = cJSON_GetObjectItemCaseSensitive(root, "network_mode");
    if (net_mode_item && cJSON_IsString(net_mode_item)) {
        if (strcmp(net_mode_item->valuestring, "wifi") == 0) {
            s.network_mode = NETWORK_MODE_WIFI_ONLY;
        } else if (strcmp(net_mode_item->valuestring, "cellular") == 0) {
            s.network_mode = NETWORK_MODE_CELLULAR_ONLY;
        } else {
            s.network_mode = NETWORK_MODE_BOTH;
        }
    }

    cJSON *gnss_uart_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_uart_num");
    if (gnss_uart_item && cJSON_IsNumber(gnss_uart_item) && gnss_uart_item->valueint >= 0 && gnss_uart_item->valueint < 3) {
        s.gnss_uart_num = gnss_uart_item->valueint;
    }
    copy_pin_field(root, "gnss_uart_tx_pin", &s.gnss_uart_tx_pin);
    copy_pin_field(root, "gnss_uart_rx_pin", &s.gnss_uart_rx_pin);
    cJSON *gnss_baud_item = cJSON_GetObjectItemCaseSensitive(root, "gnss_uart_baud");
    if (gnss_baud_item && cJSON_IsNumber(gnss_baud_item) && gnss_baud_item->valueint > 0) {
        s.gnss_uart_baud = gnss_baud_item->valueint;
    }

    cJSON *rgb_mode_item = cJSON_GetObjectItemCaseSensitive(root, "rgb_led_mode");
    if (rgb_mode_item && cJSON_IsString(rgb_mode_item)) {
        if (strcmp(rgb_mode_item->valuestring, "ws2812") == 0) {
            s.rgb_led_mode = RGB_LED_WS2812;
        } else if (strcmp(rgb_mode_item->valuestring, "pwm3") == 0) {
            s.rgb_led_mode = RGB_LED_PWM3;
        } else {
            s.rgb_led_mode = RGB_LED_NONE;
        }
    }
    copy_pin_field(root, "rgb_led_ws2812_pin", &s.rgb_led_ws2812_pin);
    copy_pin_field(root, "rgb_led_pwm_r_pin", &s.rgb_led_pwm_r_pin);
    copy_pin_field(root, "rgb_led_pwm_g_pin", &s.rgb_led_pwm_g_pin);
    copy_pin_field(root, "rgb_led_pwm_b_pin", &s.rgb_led_pwm_b_pin);
    cJSON *rgb_active_low_item = cJSON_GetObjectItemCaseSensitive(root, "rgb_led_pwm_active_low");
    if (rgb_active_low_item && cJSON_IsBool(rgb_active_low_item)) {
        s.rgb_led_pwm_active_low = cJSON_IsTrue(rgb_active_low_item);
    }

    copy_pin_field(root, "oled_sda_pin", &s.oled_sda_pin);
    copy_pin_field(root, "oled_scl_pin", &s.oled_scl_pin);
    cJSON *oled_addr_item = cJSON_GetObjectItemCaseSensitive(root, "oled_i2c_addr");
    if (oled_addr_item && cJSON_IsNumber(oled_addr_item) && oled_addr_item->valueint > 0 && oled_addr_item->valueint < 256) {
        s.oled_i2c_addr = (uint8_t) oled_addr_item->valueint;
    }
    cJSON *oled_controller_item = cJSON_GetObjectItemCaseSensitive(root, "oled_controller");
    if (oled_controller_item && cJSON_IsString(oled_controller_item)) {
        if (strcmp(oled_controller_item->valuestring, "sh1106") == 0) {
            s.oled_controller = OLED_CTRL_SH1106;
        } else if (strcmp(oled_controller_item->valuestring, "ssd1309") == 0) {
            s.oled_controller = OLED_CTRL_SSD1309;
        } else {
            s.oled_controller = OLED_CTRL_SSD1306;
        }
    }
    cJSON *oled_flip_h_item = cJSON_GetObjectItemCaseSensitive(root, "oled_flip_h");
    if (oled_flip_h_item && cJSON_IsBool(oled_flip_h_item)) {
        s.oled_flip_h = cJSON_IsTrue(oled_flip_h_item);
    }
    cJSON *oled_flip_v_item = cJSON_GetObjectItemCaseSensitive(root, "oled_flip_v");
    if (oled_flip_v_item && cJSON_IsBool(oled_flip_v_item)) {
        s.oled_flip_v = cJSON_IsTrue(oled_flip_v_item);
    }

    cJSON_Delete(root);

    esp_err_t err = settings_save(&s);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "salvataggio fallito");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

// Contesto per ota_read_from_http(): legge dal corpo della richiesta HTTP
// finche' non sono stati consumati tutti i content_len byte annunciati.
typedef struct {
    httpd_req_t *req;
    int remaining;
} http_ota_ctx_t;

static int ota_read_from_http(void *ctx_ptr, uint8_t *buf, size_t max_len)
{
    http_ota_ctx_t *ctx = (http_ota_ctx_t *) ctx_ptr;
    if (ctx->remaining <= 0) {
        return 0;
    }
    size_t to_read = max_len < (size_t) ctx->remaining ? max_len : (size_t) ctx->remaining;
    int r = httpd_req_recv(ctx->req, (char *) buf, to_read);
    if (r <= 0) {
        return -1;
    }
    ctx->remaining -= r;
    return r;
}

static esp_err_t ota_upload_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo mancante");
        return ESP_FAIL;
    }

    ESP_LOGI(TAG, "Aggiornamento firmware ricevuto dal browser (%d byte)...", req->content_len);

    http_ota_ctx_t ctx = { .req = req, .remaining = req->content_len };
    esp_err_t err = ota_update_apply(ota_read_from_http, &ctx);
    if (err != ESP_OK) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "aggiornamento fallito, immagine non applicata");
        return ESP_FAIL;
    }

    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Firmware aggiornato, riavvio in corso");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

static esp_err_t ota_sd_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char msg[96] = {0};
    bool applied = sd_update_check_and_apply(msg, sizeof(msg));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "applied", applied);
    cJSON_AddStringToObject(root, "message", msg);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);

    if (applied) {
        ESP_LOGI(TAG, "Firmware aggiornato da microSD, riavvio in corso");
        vTaskDelay(pdMS_TO_TICKS(300));
        esp_restart();
    }
    return ESP_OK;
}

// Stato del test di connessione WiFi in corso, letto dalla UI web mentre
// il tentativo gira in background - vedi wifi_test_connect_task() sotto
// per il motivo per cui non gira piu' direttamente nel task del server web.
typedef struct {
    bool running;
    bool done;
    bool connected; // valido solo se done == true
} wifi_test_status_t;

static SemaphoreHandle_t s_wifi_test_mutex;
static wifi_test_status_t s_wifi_test_status;

typedef struct {
    char ssid[33];
    char password[65];
} wifi_test_task_ctx_t;

static void wifi_test_connect_task(void *arg)
{
    wifi_test_task_ctx_t *ctx = (wifi_test_task_ctx_t *) arg;

    bool connected = wifi_link_connect_with(ctx->ssid, ctx->password, 15000);
    if (connected) {
        app_settings_t s = settings_get();
        strncpy(s.wifi_ssid, ctx->ssid, sizeof(s.wifi_ssid) - 1);
        s.wifi_ssid[sizeof(s.wifi_ssid) - 1] = '\0';
        strncpy(s.wifi_password, ctx->password, sizeof(s.wifi_password) - 1);
        s.wifi_password[sizeof(s.wifi_password) - 1] = '\0';
        settings_save(&s);
        status_set_net(NET_STATUS_WIFI);
        ESP_LOGI(TAG, "Connesso, credenziali salvate automaticamente");
    }

    xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
    s_wifi_test_status.running = false;
    s_wifi_test_status.done = true;
    s_wifi_test_status.connected = connected;
    xSemaphoreGive(s_wifi_test_mutex);

    free(ctx);
    vTaskDelete(NULL);
}

static esp_err_t wifi_test_connect_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 256) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char buf[257];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }
    cJSON *ssid_item = cJSON_GetObjectItemCaseSensitive(root, "ssid");
    if (!ssid_item || !cJSON_IsString(ssid_item) || strlen(ssid_item->valuestring) == 0) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "SSID mancante");
        return ESP_FAIL;
    }

    char ssid[33];
    strncpy(ssid, ssid_item->valuestring, sizeof(ssid) - 1);
    ssid[sizeof(ssid) - 1] = '\0';

    // Password vuota/assente = mantieni quella gia' salvata per questo
    // SSID (stessa convenzione dei campi password nel resto della UI) -
    // utile per ritestare una rete gia' configurata senza riscriverla.
    char password[65];
    cJSON *pass_item = cJSON_GetObjectItemCaseSensitive(root, "password");
    if (pass_item && cJSON_IsString(pass_item) && strlen(pass_item->valuestring) > 0) {
        strncpy(password, pass_item->valuestring, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';
    } else {
        app_settings_t existing = settings_get();
        strncpy(password, existing.wifi_password, sizeof(password) - 1);
        password[sizeof(password) - 1] = '\0';
    }
    cJSON_Delete(root);

    // Avviato in un task separato invece di bloccare qui fino a 15s: il
    // tentativo obbliga la radio a spostarsi sul canale della rete di
    // destinazione per autenticarsi, il che puo' disturbare momentaneamente
    // il collegamento della pagina stessa (sempre sull'AP di setup, canale
    // 1) - confermato su hardware reale ("spesso si blocca" durante il
    // test). Se il server web resta bloccato proprio in quel momento, la
    // richiesta HTTP puo' restare sospesa indefinitamente lato browser.
    if (!s_wifi_test_mutex) {
        s_wifi_test_mutex = xSemaphoreCreateMutex();
    }
    xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
    s_wifi_test_status = (wifi_test_status_t){ .running = true };
    xSemaphoreGive(s_wifi_test_mutex);

    wifi_test_task_ctx_t *ctx = calloc(1, sizeof(wifi_test_task_ctx_t));
    if (!ctx) {
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria esaurita");
        return ESP_FAIL;
    }
    strncpy(ctx->ssid, ssid, sizeof(ctx->ssid) - 1);
    strncpy(ctx->password, password, sizeof(ctx->password) - 1);

    ESP_LOGI(TAG, "Test connessione WiFi a '%s' avviato in background", ssid);
    if (xTaskCreate(wifi_test_connect_task, "wifi_test", 4096, ctx, 5, NULL) != pdPASS) {
        free(ctx);
        httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "avvio task fallito");
        return ESP_FAIL;
    }

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true}");
    return ESP_OK;
}

static esp_err_t wifi_test_progress_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_test_status_t st = {0};
    if (s_wifi_test_mutex) {
        xSemaphoreTake(s_wifi_test_mutex, portMAX_DELAY);
        st = s_wifi_test_status;
        xSemaphoreGive(s_wifi_test_mutex);
    }

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "running", st.running);
    cJSON_AddBoolToObject(resp, "done", st.done);
    cJSON_AddBoolToObject(resp, "connected", st.connected);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t wifi_scan_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    wifi_scan_result_t results[16];
    size_t n = wifi_link_scan(results, 16);

    cJSON *root = cJSON_CreateObject();
    cJSON *networks = cJSON_CreateArray();
    for (size_t i = 0; i < n; i++) {
        cJSON *net = cJSON_CreateObject();
        cJSON_AddStringToObject(net, "ssid", results[i].ssid);
        cJSON_AddNumberToObject(net, "rssi", results[i].rssi);
        cJSON_AddBoolToObject(net, "secure", results[i].secure);
        cJSON_AddItemToArray(networks, net);
    }
    cJSON_AddItemToObject(root, "networks", networks);

    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ota_check_online_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    char version[32] = {0};
    char url[128] = {0};
    char msg[96] = {0};
    bool available = online_update_check(version, sizeof(version), url, sizeof(url), msg, sizeof(msg));

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "available", available);
    cJSON_AddStringToObject(root, "version", version);
    cJSON_AddStringToObject(root, "url", url);
    cJSON_AddStringToObject(root, "message", msg);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t ota_apply_online_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    if (req->content_len <= 0 || req->content_len > 512) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo non valido");
        return ESP_FAIL;
    }

    char buf[513];
    int received = 0;
    while (received < req->content_len) {
        int r = httpd_req_recv(req, buf + received, req->content_len - received);
        if (r <= 0) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
            return ESP_FAIL;
        }
        received += r;
    }
    buf[received] = '\0';

    cJSON *root = cJSON_Parse(buf);
    if (!root) {
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "JSON non valido");
        return ESP_FAIL;
    }
    cJSON *url_item = cJSON_GetObjectItemCaseSensitive(root, "url");
    if (!url_item || !cJSON_IsString(url_item)) {
        cJSON_Delete(root);
        httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "campo \"url\" mancante");
        return ESP_FAIL;
    }

    char url[128];
    strncpy(url, url_item->valuestring, sizeof(url) - 1);
    url[sizeof(url) - 1] = '\0';
    cJSON_Delete(root);

    // Avviato in un task separato: il download+applicazione puo' richiedere
    // decine di secondi, e serve poter interrogare l'avanzamento
    // (/api/ota/progress) mentre e' in corso invece di restare bloccati in
    // attesa di questa risposta HTTP fino alla fine.
    online_update_apply_async(url);
    ESP_LOGI(TAG, "Aggiornamento online avviato in background");

    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, "{\"ok\":true,\"message\":\"Avviato\"}");
    return ESP_OK;
}

static esp_err_t ota_progress_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    online_update_progress_t p = online_update_get_progress();

    cJSON *root = cJSON_CreateObject();
    cJSON_AddBoolToObject(root, "running", p.running);
    cJSON_AddBoolToObject(root, "done", p.done);
    cJSON_AddBoolToObject(root, "ok", p.ok);
    cJSON_AddNumberToObject(root, "percent", p.percent);
    cJSON_AddNumberToObject(root, "bytes_read", p.bytes_read);
    cJSON_AddNumberToObject(root, "bytes_total", p.bytes_total);
    cJSON_AddStringToObject(root, "message", p.message);
    char *json = cJSON_PrintUnformatted(root);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(root);
    return ESP_OK;
}

static esp_err_t log_get_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    static char buf[8192]; // static: troppo grande per lo stack del task httpd
    size_t n = log_buffer_read(buf, sizeof(buf) - 1);
    buf[n] = '\0';

    httpd_resp_set_type(req, "text/plain; charset=utf-8");
    httpd_resp_sendstr(req, buf);
    return ESP_OK;
}

static esp_err_t alerts_test_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }

    // Usa i valori correnti del form (non serve averli gia' salvati, stesso
    // principio del pulsante "Connetti" del WiFi) - un campo assente/vuoto
    // nel corpo mantiene il valore gia' salvato (vedi copy_field()).
    app_settings_t s = settings_get();

    if (req->content_len > 0) {
        if (req->content_len > 1024) {
            httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "corpo troppo grande");
            return ESP_FAIL;
        }
        char *buf = malloc(req->content_len + 1);
        if (!buf) {
            httpd_resp_send_err(req, HTTPD_500_INTERNAL_SERVER_ERROR, "memoria esaurita");
            return ESP_FAIL;
        }
        int received = 0;
        while (received < req->content_len) {
            int r = httpd_req_recv(req, buf + received, req->content_len - received);
            if (r <= 0) {
                free(buf);
                httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "lettura corpo fallita");
                return ESP_FAIL;
            }
            received += r;
        }
        buf[received] = '\0';
        cJSON *root = cJSON_Parse(buf);
        free(buf);
        if (root) {
            copy_field(root, "alert_smtp_host", s.alert_smtp_host, sizeof(s.alert_smtp_host));
            copy_field(root, "alert_smtp_user", s.alert_smtp_user, sizeof(s.alert_smtp_user));
            copy_field(root, "alert_smtp_password", s.alert_smtp_password, sizeof(s.alert_smtp_password));
            copy_field(root, "alert_email_to", s.alert_email_to, sizeof(s.alert_email_to));
            copy_field(root, "alert_whatsapp_phone", s.alert_whatsapp_phone, sizeof(s.alert_whatsapp_phone));
            copy_field(root, "alert_whatsapp_apikey", s.alert_whatsapp_apikey, sizeof(s.alert_whatsapp_apikey));
            cJSON *port_item = cJSON_GetObjectItemCaseSensitive(root, "alert_smtp_port");
            if (port_item && cJSON_IsNumber(port_item) && port_item->valueint > 0 && port_item->valueint <= 65535) {
                s.alert_smtp_port = (uint16_t) port_item->valueint;
            }
            cJSON_Delete(root);
        }
    }

    char msg[256] = {0};
    bool ok = alerts_send_test(&s, msg, sizeof(msg));

    cJSON *resp = cJSON_CreateObject();
    cJSON_AddBoolToObject(resp, "ok", ok);
    cJSON_AddStringToObject(resp, "message", msg);
    char *json = cJSON_PrintUnformatted(resp);
    httpd_resp_set_type(req, "application/json");
    httpd_resp_sendstr(req, json);
    free(json);
    cJSON_Delete(resp);
    return ESP_OK;
}

static esp_err_t reboot_post_handler(httpd_req_t *req)
{
    if (require_auth(req) != ESP_OK) {
        return ESP_FAIL;
    }
    httpd_resp_sendstr(req, "{\"ok\":true}");
    ESP_LOGI(TAG, "Riavvio richiesto dalla UI web");
    vTaskDelay(pdMS_TO_TICKS(300));
    esp_restart();
    return ESP_OK;
}

void web_ui_start(void)
{
    httpd_config_t config = HTTPD_DEFAULT_CONFIG();
    config.max_uri_handlers = 17; // default 8, non basta piu' con gli endpoint OTA/WiFi/log/avvisi aggiunti
    // Il default (4096 byte) va in overflow quando un handler fa una
    // richiesta HTTPS in uscita (es. ota_check_online_post_handler verso
    // GitHub): l'handshake TLS/mbedTLS richiede piu' stack di quanto ne
    // serva per gestire richieste normali - confermato da un crash reale
    // su hardware (stessa causa, in un task diverso, del fix allo stack
    // del task "main" durante l'init WiFi fatto in precedenza).
    config.stack_size = 10240;

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Avvio server web fallito");
        return;
    }

    httpd_uri_t index_uri      = { .uri = "/",              .method = HTTP_GET,  .handler = index_get_handler };
    httpd_uri_t status_uri     = { .uri = "/api/status",     .method = HTTP_GET,  .handler = status_get_handler };
    httpd_uri_t signals_uri    = { .uri = "/api/signals",    .method = HTTP_GET,  .handler = signals_get_handler };
    httpd_uri_t settings_uri   = { .uri = "/api/settings",   .method = HTTP_POST, .handler = settings_post_handler };
    httpd_uri_t reboot_uri     = { .uri = "/api/reboot",     .method = HTTP_POST, .handler = reboot_post_handler };
    httpd_uri_t ota_upload_uri = { .uri = "/api/ota/upload",    .method = HTTP_POST, .handler = ota_upload_post_handler };
    httpd_uri_t ota_sd_uri     = { .uri = "/api/ota/sd-update", .method = HTTP_POST, .handler = ota_sd_post_handler };
    httpd_uri_t ota_check_uri  = { .uri = "/api/ota/check-online", .method = HTTP_POST, .handler = ota_check_online_post_handler };
    httpd_uri_t ota_apply_uri  = { .uri = "/api/ota/apply-online", .method = HTTP_POST, .handler = ota_apply_online_post_handler };
    httpd_uri_t ota_progress_uri = { .uri = "/api/ota/progress", .method = HTTP_GET, .handler = ota_progress_get_handler };
    httpd_uri_t wifi_scan_uri  = { .uri = "/api/wifi/scan", .method = HTTP_GET, .handler = wifi_scan_get_handler };
    httpd_uri_t wifi_test_uri  = { .uri = "/api/wifi/test-connect", .method = HTTP_POST, .handler = wifi_test_connect_post_handler };
    httpd_uri_t wifi_test_progress_uri = { .uri = "/api/wifi/test-progress", .method = HTTP_GET, .handler = wifi_test_progress_get_handler };
    httpd_uri_t log_uri        = { .uri = "/api/log", .method = HTTP_GET, .handler = log_get_handler };
    httpd_uri_t alerts_test_uri = { .uri = "/api/alerts/test", .method = HTTP_POST, .handler = alerts_test_post_handler };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &wifi_scan_uri);
    httpd_register_uri_handler(server, &wifi_test_uri);
    httpd_register_uri_handler(server, &wifi_test_progress_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &signals_uri);
    httpd_register_uri_handler(server, &settings_uri);
    httpd_register_uri_handler(server, &reboot_uri);
    httpd_register_uri_handler(server, &ota_upload_uri);
    httpd_register_uri_handler(server, &ota_sd_uri);
    httpd_register_uri_handler(server, &ota_check_uri);
    httpd_register_uri_handler(server, &ota_apply_uri);
    httpd_register_uri_handler(server, &ota_progress_uri);
    httpd_register_uri_handler(server, &log_uri);
    httpd_register_uri_handler(server, &alerts_test_uri);

    ESP_LOGI(TAG, "Server web di gestione avviato");
}
