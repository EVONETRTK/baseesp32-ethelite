#include "gnss_comnav.h"
#include "gnss_io.h"
#include "settings.h"
#include "base_measure.h"

#include <ctype.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "nvs.h"

static const char *TAG = "gnss_comnav";

static char s_model[24];
static char s_fw[32];
static bool s_is_comnav;

static esp_err_t send_cmd(uart_port_t uart_num, const char *cmd)
{
    ESP_LOGI(TAG, "-> %s", cmd);
    int len = (int) strlen(cmd);
    int written = gnss_io_write(cmd, len);
    gnss_io_write("\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}

// "LOG RTCM<tipo>B ONTIME <s>" sulla porta da cui arriva il comando (senza
// porta = porta corrente, manuale cap. 5.3). Solo se scelto nel pannello:
// UNLOGALL ha gia' spento il resto.
static esp_err_t send_rtcm_if(uart_port_t uart_num, bool enabled, int rtcm_type, int ontime_s)
{
    if (!enabled) {
        return ESP_OK;
    }
    char cmd[40];
    snprintf(cmd, sizeof(cmd), "LOG RTCM%dB ONTIME %d", rtcm_type, ontime_s);
    return send_cmd(uart_num, cmd);
}

// Stringhe tra virgolette di una riga, in ordine.
static int quoted(const char *s, char out[][32], int max)
{
    int n = 0;
    while (n < max && (s = strchr(s, '"')) != NULL) {
        const char *e = strchr(s + 1, '"');
        if (!e) {
            break;
        }
        size_t k = (size_t) (e - s - 1);
        if (k > 31) k = 31;
        memcpy(out[n], s + 1, k);
        out[n][k] = '\0';
        n++;
        s = e + 1;
    }
    return n;
}

void gnss_comnav_note_line(const char *line)
{
    // #VERSIONA,...;1,GPSCARD,"S2002","00902165","CARD-501AA-22","1.10A-1.10A",...
    // (struttura dal manuale: modello, numero di serie, hardware, firmware).
    if (strncmp(line, "#VERSIONA", 9) != 0 || strstr(line, "\"UM9")) {
        return; // gli Unicore li legge gnss_unicore.c
    }
    char q[6][32] = {{0}};
    int n = quoted(line, q, 6);
    if (n < 1) {
        return;
    }
    strlcpy(s_fw, n >= 4 ? q[3] : "", sizeof(s_fw));
    strlcpy(s_model, q[0], sizeof(s_model));
    char up[256];
    size_t k = 0;
    for (; line[k] && k < sizeof(up) - 1; k++) up[k] = (char) toupper((unsigned char) line[k]);
    up[k] = '\0';
    s_is_comnav = (s_model[0] == 'K' && isdigit((unsigned char) s_model[1])) ||
                  strstr(up, "COMNAV") || strstr(up, "SINOGNSS") || strstr(up, "\"K9") || strstr(up, "\"K8");
    ESP_LOGI(TAG, "Risposta VERSIONA: modello \"%s\", firmware \"%s\"%s", s_model, s_fw,
             s_is_comnav ? " (ComNav)" : "");
}

const char *gnss_comnav_model(void)
{
    return s_model;
}

const char *gnss_comnav_fw(void)
{
    return s_fw;
}

bool gnss_comnav_looks_comnav(void)
{
    return s_is_comnav;
}

static void wait_model(uart_port_t uart_num)
{
    send_cmd(uart_num, "LOG VERSIONA ONCE");
    for (int i = 0; i < 20 && !s_model[0]; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_model[0]) {
        ESP_LOGW(TAG, "Nessuna risposta a LOG VERSIONA: modello sconosciuto (velocita' della UART giusta?)");
    }
}

esp_err_t gnss_comnav_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore ComNav come base RTK (RTCM3 + NMEA per lo stato)");

    app_settings_t s; // task main (stack grande), come gli altri driver
    settings_get_into(&s);

    wait_model(uart_num);

    esp_err_t err = send_cmd(uart_num, "UNLOGALL");
    // Quota ellissoidica: con ondulazione 0 la "quota sul livello del mare"
    // di FIX POSITION e dei log coincide con quella ellissoidica del pannello.
    esp_err_t e = send_cmd(uart_num, "UNDULATION USER 0");
    if (e != ESP_OK) err = e;

    char cmd[96];
    if (s.base_position_mode == BASE_POSITION_MANUAL) {
        snprintf(cmd, sizeof(cmd), "FIX POSITION %.9f %.9f %.4f",
                 s.base_fixed_lat_deg, s.base_fixed_lon_deg, s.base_fixed_height_m);
        ESP_LOGI(TAG, "Posizione base fissa: lat %.9f lon %.9f quota ellissoidica %.4f m",
                 s.base_fixed_lat_deg, s.base_fixed_lon_deg, s.base_fixed_height_m);
    } else {
        // Il manuale non prevede durata e precisione del survey-in: FIX AUTO.
        strlcpy(cmd, "FIX AUTO", sizeof(cmd));
    }
    e = send_cmd(uart_num, cmd);
    if (e != ESP_OK) err = e;

    // Posizione della base e descrittore ogni 10 s, osservazioni ogni secondo.
    e = send_rtcm_if(uart_num, s.rtcm_1005_enable, 1005, 10); if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, true, 1033, 10);                if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1007_enable, 1007, 10); if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1008_enable, 1008, 10); if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1230_enable, 1230, 10); if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1019_enable, 1019, 30); if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1020_enable, 1020, 30); if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1074_enable, 1074, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1077_enable, 1077, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1084_enable, 1084, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1087_enable, 1087, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1094_enable, 1094, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1097_enable, 1097, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1124_enable, 1124, 1);  if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1127_enable, 1127, 1);  if (e != ESP_OK) err = e;

    static const char *const extra[] = {
        // GGA e GSV: stato del fix e grafico dei satelliti nel pannello
        // (base_stream_demux.c li separa dagli RTCM).
        "LOG GPGGA ONTIME 1",
        "LOG GPGSV ONTIME 1",
        "SAVECONFIG",
    };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        e = send_cmd(uart_num, extra[i]);
        if (e != ESP_OK) err = e;
    }
    return err;
}

typedef struct {
    bool has;
    bool ins;
    uint8_t axes;
    bool heading;
} rover_view_t;

static void peek_rover(const app_settings_t *s, void *ctx)
{
    rover_view_t *v = ctx;
    v->has = s->rover_has_fallback;
    v->ins = s->comnav_ins_enable;
    v->axes = (s->comnav_imu_axes >= 1 && s->comnav_imu_axes <= 8) ? s->comnav_imu_axes : 1;
    v->heading = s->comnav_heading_enable;
}

// INS (IMU a bordo), dal "K-series OEM Board User Guide" cap. 4.6: dopo il
// tipo di assi il modulo va riavviato. Si riavvia solo se la configurazione
// INS e' cambiata dall'ultima volta (salvata in NVS), non a ogni avvio.
#define COMNAV_NVS_NS "comnav"

static bool ins_changed(uint8_t want)
{
    nvs_handle_t h;
    uint8_t last = 0xFF;
    if (nvs_open(COMNAV_NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
        return true;
    }
    nvs_get_u8(h, "ins", &last);
    bool changed = last != want;
    if (changed) {
        nvs_set_u8(h, "ins", want);
        nvs_commit(h);
    }
    nvs_close(h);
    return changed;
}

esp_err_t gnss_comnav_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore ComNav come rover (riceve RTCM3, emette GGA)");

    wait_model(uart_num);

    rover_view_t v;
    settings_peek(peek_rover, &v);
    bool measuring = base_measure_is_has();
    bool has = measuring || v.has;
    bool ins = v.ins && !measuring;     // misura della base: antenna ferma, niente INS
    bool heading = v.heading && !measuring;

    static const char *const cmds[] = {
        "UNLOGALL",
        "FIX NONE",            // via la posizione fissa da base
        "UNDULATION USER 0",   // quote ellissoidiche, come in base
        "LOG GPGSV ONTIME 1",
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t e = send_cmd(uart_num, cmds[i]);
        if (e != ESP_OK) err = e;
    }
    // Con l'INS posizione a 5 Hz (RTK e PVT a 5 Hz, come nel manuale).
    send_cmd(uart_num, ins ? "LOG GPGGA ONTIME 0.2" : "LOG GPGGA ONTIME 1");

    // Prua a doppia antenna: HEADINGA per il pannello (stesso formato
    // NovAtel gia' letto per il Bynav M21D), GPHDT per AgOpenGPS.
    if (heading) {
        send_cmd(uart_num, "LOG HEADINGA ONTIME 1");
        send_cmd(uart_num, "LOG GPHDT ONTIME 0.2");
    }

    char cmd[40];
    if (ins) {
        // INS: posizione anche durante brevi perdite del segnale (GGA
        // qualita' 6), solo con l'IMU a bordo.
        send_cmd(uart_num, "INSCONTROL ENABLE");
        snprintf(cmd, sizeof(cmd), "SET IMUAXESTYPE %u", (unsigned) v.axes);
        send_cmd(uart_num, cmd);
        send_cmd(uart_num, "SET RTKFREQ 5");
        send_cmd(uart_num, "SET PVTFREQ 5");
        send_cmd(uart_num, "SET SMOOTHEDDR ON");
    } else {
        send_cmd(uart_num, "INSCONTROL DISABLE");
    }
    if (has) {
        // La soluzione HAS si legge da BESTPOSA (tipo PPP_CONVERGING/PPP),
        // come il PPPNAVA dell'UM982. Il comando che ATTIVA HAS sul K922 non
        // e' nel manuale disponibile: da aggiungere quando il modulo arriva.
        ESP_LOGW(TAG, "Galileo HAS richiesto: leggo BESTPOSA, ma il comando di attivazione HAS del K922 "
                      "non e' ancora noto (manuale ComNav 2019)");
        send_cmd(uart_num, "LOG BESTPOSA ONTIME 1");
    }
    esp_err_t e = send_cmd(uart_num, "SAVECONFIG");
    if (e != ESP_OK) err = e;

    // Il tipo di assi vale solo dopo un riavvio del modulo (manuale): una
    // volta, quando la configurazione INS cambia. Riparte con quella salvata.
    if (ins_changed(ins ? v.axes : 0)) {
        ESP_LOGW(TAG, "Configurazione INS cambiata: riavvio del modulo ComNav");
        send_cmd(uart_num, "RESET");
        vTaskDelay(pdMS_TO_TICKS(5000));
    }
    return err;
}
