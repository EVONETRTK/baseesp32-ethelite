#include "gnss_unicore.h"
#include "gnss_io.h"
#include "settings.h"
#include "base_measure.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gnss_unicore";

// Modello e firmware dalla risposta a VERSIONA (vedi gnss_unicore_note_line).
static char s_model[16];
static char s_fw[32];

static esp_err_t send_cmd(uart_port_t uart_num, const char *cmd)
{
    ESP_LOGI(TAG, "-> %s", cmd);
    int len = (int) strlen(cmd);
    int written = gnss_io_write(cmd, len);
    gnss_io_write("\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}

// Manda "RTCM<numero> 1" se enabled, altrimenti non fa nulla (UNLOG in
// gnss_unicore_configure_base() azzera gia' tutto, non serve un "0"
// esplicito per un messaggio non voluto). Stessa forma dei file di
// configurazione UM980/UM982 della base Raspberry (ELT_RTKBase,
// Install/UM98x_RTCM3_OUT.txt, usati sul campo).
static esp_err_t send_rtcm_if(uart_port_t uart_num, bool enabled, int rtcm_type)
{
    if (!enabled) {
        return ESP_OK;
    }
    char cmd[24];
    snprintf(cmd, sizeof(cmd), "RTCM%d 1", rtcm_type);
    return send_cmd(uart_num, cmd);
}

// Copia il campo n (da 0) di una lista separata da virgole, senza virgolette.
static void copy_field(const char *p, int n, char *out, size_t out_size)
{
    for (; n > 0 && p; n--) {
        p = strchr(p, ',');
        if (p) p++;
    }
    size_t k = 0;
    for (; p && *p && *p != ',' && *p != '*' && k < out_size - 1; p++) {
        if (*p != '"') {
            out[k++] = *p;
        }
    }
    out[k] = '\0';
}

// Ultima soluzione PPP (#PPPNAVA), scritta dal task che legge il ricevitore.
static portMUX_TYPE s_ppp_lock = portMUX_INITIALIZER_UNLOCKED;
static gnss_unicore_ppp_t s_ppp;

// #PPPNAVA,<intestazione>;SOL_COMPUTED,PPP_CONVERGING,lat,lon,quota,
// ondulazione,datum,sigma lat,sigma lon,sigma quota,"stazione",eta'
// correzioni,eta' soluzione,satelliti tracciati,satelliti usati,...*crc
// (campi verificati sui dati dell'UM982 del 07/10/2026).
static void note_pppnav(const char *data)
{
    char f[16][24];
    for (int i = 0; i < 16; i++) {
        copy_field(data, i, f[i], sizeof(f[i]));
    }
    gnss_unicore_ppp_t p = {0};
    p.valid = strcmp(f[0], "SOL_COMPUTED") == 0 && strncmp(f[1], "PPP", 3) == 0;
    strlcpy(p.type, f[1], sizeof(p.type));
    p.lat_deg = atof(f[2]);
    p.lon_deg = atof(f[3]);
    p.h_ell_m = atof(f[4]) + atof(f[5]);
    p.sig_lat_m = (float) atof(f[7]);
    p.sig_lon_m = (float) atof(f[8]);
    p.sig_h_m = (float) atof(f[9]);
    p.corr_age_s = (float) atof(f[11]);
    p.sats_used = atoi(f[14]);
    p.at_us = esp_timer_get_time();
    portENTER_CRITICAL(&s_ppp_lock);
    s_ppp = p;
    portEXIT_CRITICAL(&s_ppp_lock);
}

void gnss_unicore_ppp_get(gnss_unicore_ppp_t *out)
{
    portENTER_CRITICAL(&s_ppp_lock);
    *out = s_ppp;
    portEXIT_CRITICAL(&s_ppp_lock);
}

void gnss_unicore_note_line(const char *line)
{
    // PPPNAVA (Unicore) e BESTPOSA (stile NovAtel: ComNav K922) hanno gli
    // stessi campi; note_pppnav tiene solo le soluzioni di tipo PPP_*.
    if (strncmp(line, "#PPPNAVA", 8) == 0 || strncmp(line, "#BESTPOSA", 9) == 0) {
        const char *d = strchr(line, ';');
        if (d) {
            note_pppnav(d + 1);
        }
        return;
    }
    // #VERSIONA,<intestazione>;"UM982","R4.10Build7923",...*crc
    if (strncmp(line, "#VERSIONA", 9) != 0) {
        return;
    }
    const char *data = strchr(line, ';');
    if (!data) {
        return;
    }
    char model[sizeof(s_model)], fw[sizeof(s_fw)];
    copy_field(data + 1, 0, model, sizeof(model));
    copy_field(data + 1, 1, fw, sizeof(fw));
    if (strncmp(model, "UM", 2) == 0) { // solo Unicore: un ComNav risponde con un'altra struttura
        strlcpy(s_fw, fw, sizeof(s_fw));
        strlcpy(s_model, model, sizeof(s_model)); // per ultimo: chi aspetta il modello trova anche il firmware
        ESP_LOGI(TAG, "Ricevitore Unicore: %s, firmware %s", s_model, s_fw);
    }
}

const char *gnss_unicore_model(void)
{
    return s_model;
}

const char *gnss_unicore_fw(void)
{
    return s_fw;
}

// Chiede la versione e aspetta fino a 2 s il modello (la risposta la legge
// il task della UART e la passa a gnss_unicore_note_line).
static void wait_model(uart_port_t uart_num)
{
    send_cmd(uart_num, "VERSIONA");
    for (int i = 0; i < 20 && !s_model[0]; i++) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    if (!s_model[0]) {
        ESP_LOGW(TAG, "Nessuna risposta a VERSIONA: modello sconosciuto (velocita' della UART giusta? "
                      "UM98x di fabbrica: 115200)");
    }
}

// Gruppo di segnali per modello, come nella base Raspberry: UM982
// "SIGNALGROUP 7 0", UM980 "SIGNALGROUP 2". Con un modello sconosciuto si
// lascia quello del ricevitore. Il cambio puo' far ripartire il ricevitore:
// si aspetta prima di continuare.
static void set_signal_group(uart_port_t uart_num)
{
    const char *cmd = NULL;
    if (strstr(s_model, "UM982")) {
        cmd = "CONFIG SIGNALGROUP 7 0";
    } else if (strstr(s_model, "UM980")) {
        cmd = "CONFIG SIGNALGROUP 2";
    }
    if (!cmd) {
        return;
    }
    send_cmd(uart_num, cmd);
    vTaskDelay(pdMS_TO_TICKS(5000));
    send_cmd(uart_num, "UNLOG");
}

esp_err_t gnss_unicore_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore come base RTK (RTCM3 + NMEA per lo stato)");

    app_settings_t s; // task main (stack grande), come gnss_ubx_configure_base
    settings_get_into(&s);

    wait_model(uart_num);

    // UNLOG azzera tutti i log/messaggi attivi: non serve disattivare
    // esplicitamente un messaggio non scelto, parte gia' da zero.
    esp_err_t err = send_cmd(uart_num, "UNLOG");
    set_signal_group(uart_num);

    // Quote ellissoidiche (ondulazione del geoide 0), come la quota fissa
    // del pannello e come nella base Raspberry.
    esp_err_t e = send_cmd(uart_num, "CONFIG UNDULATION 0");
    if (e != ESP_OK) err = e;

    char mode_cmd[96];
    if (s.base_position_mode == BASE_POSITION_MANUAL) {
        // Posizione fissa nota: la base manda subito il 1005 con queste
        // coordinate. Stessa forma della base Raspberry (UnicoreSetBasePos.sh).
        snprintf(mode_cmd, sizeof(mode_cmd), "MODE BASE 1 %.9f %.9f %.4f",
                 s.base_fixed_lat_deg, s.base_fixed_lon_deg, s.base_fixed_height_m);
        ESP_LOGI(TAG, "Posizione base fissa: lat %.9f lon %.9f quota ellissoidica %.4f m",
                 s.base_fixed_lat_deg, s.base_fixed_lon_deg, s.base_fixed_height_m);
    } else {
        // Durata e precisione del survey-in dal pannello (0 = predefinito: 60 s, 2,5 m).
        snprintf(mode_cmd, sizeof(mode_cmd), "MODE BASE 1 TIME %u %.2f",
                 (unsigned) (s.base_svin_min_dur_s ? s.base_svin_min_dur_s : 60),
                 (double) (s.base_svin_acc_m > 0 ? s.base_svin_acc_m : 2.5f));
    }
    e = send_cmd(uart_num, mode_cmd);
    if (e != ESP_OK) err = e;

    e = send_rtcm_if(uart_num, s.rtcm_1005_enable, 1005);
    if (e != ESP_OK) err = e;
    e = send_cmd(uart_num, "RTCM1033 1"); // descrittore antenna+ricevitore Unicore, sempre utile, non fa parte della selezione (diverso da 1007/1008)
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1230_enable, 1230);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1007_enable, 1007);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1008_enable, 1008);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1019_enable, 1019);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1020_enable, 1020);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1074_enable, 1074);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1077_enable, 1077);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1084_enable, 1084);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1087_enable, 1087);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1094_enable, 1094);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1097_enable, 1097);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1124_enable, 1124);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1127_enable, 1127);
    if (e != ESP_OK) err = e;

    // Come nella base Raspberry: niente SBAS su una base, riduzione dei
    // multipercorsi, nessuna correzione dell'orologio negli RTCM.
    static const char *const extra[] = {
        "CONFIG SBAS DISABLE",
        "CONFIG MMP ENABLE",
        "CONFIG RTCMCLOCKOFFSET DISABLE",
        // GGA e GSV ogni secondo: stato del fix e grafico dei satelliti nel
        // pannello (base_stream_demux.c li separa dagli RTCM).
        "GPGGA 1",
        "GPGSV 1",
        "SAVECONFIG",
    };
    for (size_t i = 0; i < sizeof(extra) / sizeof(extra[0]); i++) {
        e = send_cmd(uart_num, extra[i]);
        if (e != ESP_OK) err = e;
    }

    return err;
}

// Misura della posizione della base con Galileo HAS (base_measure.c): PPP
// con le correzioni HAS dal segnale E6, come lo strumento HAS della base
// Raspberry (ELT_RTKBase, Windows/UM98x_HAS.txt). Niente SAVECONFIG: al
// ritorno come base gnss_unicore_configure_base() rimette e salva tutto,
// compreso il gruppo di segnali da base.
static esp_err_t configure_has(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore per la misura con Galileo HAS (PPP, segnale E6)");
    send_cmd(uart_num, "UNLOG");
    if (strstr(s_model, "UM982")) {
        // L'UM982 riceve E6 solo con questo gruppo di segnali.
        send_cmd(uart_num, "CONFIG SIGNALGROUP 3 6");
        vTaskDelay(pdMS_TO_TICKS(5000));
        send_cmd(uart_num, "UNLOG");
    }
    static const char *const cmds[] = {
        "CONFIG PPP ENABLE E6-HAS",
        "CONFIG PPP DATUM WGS84",
        "MODE ROVER SURVEY DEFAULT",
        "GPGGA 1",
        "GPGSV 1",
        "PPPNAVA 1", // la soluzione HAS esce solo qui, non nella GGA (prova del 07/10)
    };
    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t e = send_cmd(uart_num, cmds[i]);
        if (e != ESP_OK) {
            err = e;
        }
    }
    return err;
}

static void peek_rover_has(const app_settings_t *s, void *ctx)
{
    *(bool *) ctx = s->rover_has_fallback;
}

esp_err_t gnss_unicore_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Unicore come rover (riceve RTCM3, emette $GxGGA)");

    wait_model(uart_num);
    if (base_measure_is_has()) {
        return configure_has(uart_num);
    }

    // Riserva Galileo HAS: il ricevitore usa l'RTK quando arrivano le
    // correzioni dalla base, altrimenti il PPP con HAS (segnale E6). Le
    // posizioni fuori dall'RTK le converte in ETRF2000 gnss_nmea_reader.c.
    // Senza riserva: PPP spento e gruppo di segnali normale, cosi' una
    // riserva attivata prima (salvata nel ricevitore) viene tolta.
    bool has = false;
    settings_peek(peek_rover_has, &has);
    send_cmd(uart_num, "UNLOG");
    if (has) {
        ESP_LOGI(TAG, "Riserva Galileo HAS attiva: PPP con E6-HAS quando manca l'RTK");
        if (strstr(s_model, "UM982")) {
            send_cmd(uart_num, "CONFIG SIGNALGROUP 3 6"); // E6 solo con questo gruppo
            vTaskDelay(pdMS_TO_TICKS(5000));
            send_cmd(uart_num, "UNLOG");
        }
        send_cmd(uart_num, "CONFIG PPP ENABLE E6-HAS");
        send_cmd(uart_num, "CONFIG PPP DATUM WGS84");
        send_cmd(uart_num, "PPPNAVA 1"); // soluzione HAS: solo qui, non nella GGA
    } else {
        set_signal_group(uart_num);
        send_cmd(uart_num, "CONFIG PPP DISABLE");
    }

    // Modalita' rover standard Unicore UM98x, con uscita NMEA GGA a 1Hz
    // (usata da ntrip_rover_client.c per inoltrare la posizione al caster).
    static const char *const cmds[] = {
        "MODE ROVER",
        "GPGGA 1",
        "GPGSV 1",
        "SAVECONFIG",
    };

    esp_err_t err = ESP_OK;
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        esp_err_t e = send_cmd(uart_num, cmds[i]);
        if (e != ESP_OK) {
            err = e;
        }
    }
    return err;
}
