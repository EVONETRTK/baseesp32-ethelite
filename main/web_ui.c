#include "web_ui.h"
#include "settings.h"
#include "status.h"
#include "gnss_signal.h"
#include "wifi_link.h"
#include "cellular_link.h"

#include <string.h>
#include <stdlib.h>

#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_system.h"
#include "cJSON.h"
#include "mbedtls/base64.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

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

static esp_err_t require_auth(httpd_req_t *req)
{
    if (check_auth(req)) {
        return ESP_OK;
    }
    httpd_resp_set_status(req, "401 Unauthorized");
    httpd_resp_set_hdr(req, "WWW-Authenticate", "Basic realm=\"baseesp32\"");
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
    return c == GNSS_CHIP_UNICORE ? "unicore" : "ublox";
}

static const char *device_mode_str(device_mode_t m)
{
    return m == DEVICE_MODE_ROVER ? "rover" : "base";
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
    cJSON_AddNumberToObject(root, "nmea_udp_port", s.nmea_udp_port);
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
    cJSON_AddBoolToObject(root, "oled_is_sh1106", s.oled_is_sh1106);
    cJSON_AddBoolToObject(root, "oled_flip_h", s.oled_flip_h);
    cJSON_AddBoolToObject(root, "oled_flip_v", s.oled_flip_v);

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
        s.gnss_chip = (strcmp(chip_item->valuestring, "unicore") == 0) ? GNSS_CHIP_UNICORE : GNSS_CHIP_UBLOX;
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
    cJSON *oled_sh1106_item = cJSON_GetObjectItemCaseSensitive(root, "oled_is_sh1106");
    if (oled_sh1106_item && cJSON_IsBool(oled_sh1106_item)) {
        s.oled_is_sh1106 = cJSON_IsTrue(oled_sh1106_item);
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

    httpd_handle_t server = NULL;
    if (httpd_start(&server, &config) != ESP_OK) {
        ESP_LOGE(TAG, "Avvio server web fallito");
        return;
    }

    httpd_uri_t index_uri    = { .uri = "/",              .method = HTTP_GET,  .handler = index_get_handler };
    httpd_uri_t status_uri   = { .uri = "/api/status",     .method = HTTP_GET,  .handler = status_get_handler };
    httpd_uri_t signals_uri  = { .uri = "/api/signals",    .method = HTTP_GET,  .handler = signals_get_handler };
    httpd_uri_t settings_uri = { .uri = "/api/settings",   .method = HTTP_POST, .handler = settings_post_handler };
    httpd_uri_t reboot_uri   = { .uri = "/api/reboot",     .method = HTTP_POST, .handler = reboot_post_handler };

    httpd_register_uri_handler(server, &index_uri);
    httpd_register_uri_handler(server, &status_uri);
    httpd_register_uri_handler(server, &signals_uri);
    httpd_register_uri_handler(server, &settings_uri);
    httpd_register_uri_handler(server, &reboot_uri);

    ESP_LOGI(TAG, "Server web di gestione avviato");
}
