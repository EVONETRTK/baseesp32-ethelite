#include "gnss_bynav.h"
#include "settings.h"

#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"

static const char *TAG = "gnss_bynav";

// Numero di porta COM del modulo Bynav usato sia per inviare i comandi sia
// come uscita RTCM3/NMEA: presuppone che l'ESP32 sia cablato su UART1 del
// modulo (COM1), come nell'esempio "Step-by-Step Configuration" del
// manuale ufficiale AN085. NON confermato: dipende dal cablaggio reale
// scelto quando si progetta la scheda con questo modulo - da verificare/
// correggere quando l'hardware sara' disponibile (vedi gnss_bynav.h).
#define BYNAV_COM_PORT 1

static esp_err_t send_cmd(uart_port_t uart_num, const char *cmd)
{
    ESP_LOGI(TAG, "-> %s", cmd);
    int len = (int) strlen(cmd);
    int written = uart_write_bytes(uart_num, cmd, len);
    uart_write_bytes(uart_num, "\r\n", 2);
    vTaskDelay(pdMS_TO_TICKS(100));
    return (written == len) ? ESP_OK : ESP_FAIL;
}

// Manda "LOG COM<BYNAV_COM_PORT> RTCM<tipo> ONTIME <secondi>" se enabled,
// altrimenti non fa nulla (UNLOGALL in gnss_bynav_configure_base() azzera
// gia' tutto). Comando e numeri di messaggio RTCM3 confermati dal manuale
// ufficiale "AN085_Set-up and Configuration for Base and Rover" per
// 1074/1084/1094/1124/1006/1033; gli altri numeri (1230/1007/1008/1019/
// 1020/1077/1087/1097/1127) seguono lo stesso schema standard RTCM3.3
// dichiarato supportato dal modulo (vedi datasheet, tabella protocolli)
// ma non confermati singolarmente riga per riga sul manuale - stessa
// convenzione gia' usata in gnss_unicore.c per lo stesso motivo.
static esp_err_t send_rtcm_if(uart_port_t uart_num, bool enabled, int rtcm_type, double ontime_s)
{
    if (!enabled) {
        return ESP_OK;
    }
    char cmd[48];
    snprintf(cmd, sizeof(cmd), "LOG COM%d RTCM%d ONTIME %g", BYNAV_COM_PORT, rtcm_type, ontime_s);
    return send_cmd(uart_num, cmd);
}

esp_err_t gnss_bynav_configure_base(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Bynav come base RTK (survey-in automatico + RTCM3)");

    app_settings_t s = settings_get();

    // UNLOGALL azzera tutte le uscite gia' configurate sulla porta indicata:
    // non serve disattivare esplicitamente un messaggio non scelto.
    char unlog_cmd[24];
    snprintf(unlog_cmd, sizeof(unlog_cmd), "UNLOGALL COM%d", BYNAV_COM_PORT);
    esp_err_t err = send_cmd(uart_num, unlog_cmd);

    esp_err_t e = send_cmd(uart_num, "RTKTYPE BASE");
    if (e != ESP_OK) err = e;

    // Posizione stazione (1005/1006): il manuale ufficiale usa 1006
    // (posizione + altezza antenna) nell'esempio, non 1005 - qui seguiamo
    // la selezione utente sul campo "1005" delle impostazioni (che nel
    // resto del progetto rappresenta genericamente "posizione stazione")
    // ma mandiamo il comando RTCM1006 realmente documentato per Bynav.
    if (s.rtcm_1005_enable) {
        e = send_rtcm_if(uart_num, true, 1006, 5);
        if (e != ESP_OK) err = e;
    }
    e = send_rtcm_if(uart_num, true, 1033, 10); // descrittore antenna+ricevitore, sempre utile, come gia' fatto per Unicore
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1230_enable, 1230, 5);
    if (e != ESP_OK) err = e;
    if (s.rtcm_1007_enable || s.rtcm_1008_enable || s.rtcm_1019_enable || s.rtcm_1020_enable) {
        ESP_LOGW(TAG, "1007/1008/1019/1020 richiesti ma non confermati per Bynav M20/M20D: non inviati");
    }
    e = send_rtcm_if(uart_num, s.rtcm_1074_enable, 1074, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1077_enable, 1077, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1084_enable, 1084, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1087_enable, 1087, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1094_enable, 1094, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1097_enable, 1097, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1124_enable, 1124, 1);
    if (e != ESP_OK) err = e;
    e = send_rtcm_if(uart_num, s.rtcm_1127_enable, 1127, 1);
    if (e != ESP_OK) err = e;

    // Survey-in automatico della posizione base (equivalente a Survey-In
    // u-blox/MODE BASE TIME Unicore): il manuale consiglia di inviarlo
    // dopo almeno 2 minuti di accensione con antenna gia' collegata,
    // vincolo che questa funzione (chiamata una sola volta alla
    // configurazione) non puo' garantire da sola - da rivedere quando
    // l'hardware sara' disponibile per verificare il comportamento reale.
    e = send_cmd(uart_num, "FIX AUTO");
    if (e != ESP_OK) err = e;

    e = send_cmd(uart_num, "SAVECONFIG");
    if (e != ESP_OK) err = e;

    return err;
}

esp_err_t gnss_bynav_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Bynav come rover (riceve RTCM3, emette $GPGGA)");

    char unlog_cmd[24];
    snprintf(unlog_cmd, sizeof(unlog_cmd), "UNLOGALL COM%d", BYNAV_COM_PORT);
    esp_err_t err = send_cmd(uart_num, unlog_cmd);

    esp_err_t e = send_cmd(uart_num, "RTKTYPE ROVER");
    if (e != ESP_OK) err = e;

    char gga_cmd[40];
    snprintf(gga_cmd, sizeof(gga_cmd), "LOG COM%d GPGGA ONTIME 1", BYNAV_COM_PORT);
    e = send_cmd(uart_num, gga_cmd);
    if (e != ESP_OK) err = e;

    e = send_cmd(uart_num, "SAVECONFIG");
    if (e != ESP_OK) err = e;

    return err;
}

// Manda "SETINSTRANSLATION <antenna> X Y Z 0.05 0.05 0.05 VEHICLE" con i
// valori misurati sull'installazione reale (vedi app_settings_t in
// settings.h). 0.05/0.05/0.05 = deviazione standard della misura (5cm),
// stesso valore usato in tutti gli esempi del manuale ufficiale AN065 per
// una misura a nastro metrico - non e' un placeholder nostro.
static esp_err_t send_lever_arm(uart_port_t uart_num, const char *antenna, float x, float y, float z)
{
    // %.3f invece di %g: larghezza prevedibile (evita l'avviso -Wformat-
    // truncation del compilatore, che per %g su un double assume nel
    // caso peggiore molte piu' cifre di quante servano per un braccio di
    // leva reale, dell'ordine del metro).
    char cmd[96];
    snprintf(cmd, sizeof(cmd), "SETINSTRANSLATION %s %.3f %.3f %.3f 0.05 0.05 0.05 VEHICLE", antenna, (double) x, (double) y, (double) z);
    return send_cmd(uart_num, cmd);
}

esp_err_t gnss_bynav_m21d_configure_rover(uart_port_t uart_num)
{
    ESP_LOGI(TAG, "Configuro ricevitore Bynav M21D come rover INS (lever arm, RBV, log #INSPVAXA/#HEADINGA + $GPGGA)");

    app_settings_t s = settings_get();

    char unlog_cmd[24];
    snprintf(unlog_cmd, sizeof(unlog_cmd), "UNLOGALL COM%d", BYNAV_COM_PORT);
    esp_err_t err = send_cmd(uart_num, unlog_cmd);

    esp_err_t e = send_cmd(uart_num, "RTKTYPE ROVER");
    if (e != ESP_OK) err = e;

    // Braccio di leva delle due antenne rispetto al centro veicolo - se mai
    // misurati (tutti a zero) equivalgono a dichiarare le antenne nel
    // centro veicolo, quasi certamente sbagliato: avvisiamo nei log invece
    // di mandare silenziosamente valori palesemente non misurati.
    if (s.bynav_ant1_x_m == 0.0f && s.bynav_ant1_y_m == 0.0f && s.bynav_ant1_z_m == 0.0f) {
        ESP_LOGW(TAG, "Lever arm ANT1 non impostato (tutto zero) - misurare sull'installazione reale prima dell'uso, vedi AN065 sez. 1.4");
    }
    e = send_lever_arm(uart_num, "ANT1", s.bynav_ant1_x_m, s.bynav_ant1_y_m, s.bynav_ant1_z_m);
    if (e != ESP_OK) err = e;
    e = send_lever_arm(uart_num, "ANT2", s.bynav_ant2_x_m, s.bynav_ant2_y_m, s.bynav_ant2_z_m);
    if (e != ESP_OK) err = e;

    // Orientamento del modulo rispetto al veicolo (RBV) - 0/0/0 e' corretto
    // solo per l'installazione raccomandata (asse Y parallelo al senso di
    // marcia, vicino al centro dell'assale posteriore); altre installazioni
    // richiedono valori diversi (vedi Appendix A.1 di AN065, tool Byoffset
    // di BY_Connect) e/o la calibrazione dinamica INSCALIBRATE RBV (non
    // implementata qui: richiede guida reale su strada, non automatizzabile
    // dal firmware - vedi AN065 sez. 1.10).
    char rbv_cmd[96];
    snprintf(rbv_cmd, sizeof(rbv_cmd), "SETINSROTATION RBV %.2f %.2f %.2f 0.05 0.05 0.05",
             (double) s.bynav_rbv_roll_deg, (double) s.bynav_rbv_pitch_deg, (double) s.bynav_rbv_yaw_deg);
    e = send_cmd(uart_num, rbv_cmd);
    if (e != ESP_OK) err = e;

    // Log INS (letti da gnss_nmea_reader.c) + GGA per il caster, tutti
    // sulla stessa porta/UART (nessuna porta COM separata cablata).
    const char *logs[] = {
        "BESTPOSA", "HEADINGA", "INSPVAXA", "INSCALSTATUSA", "GPGGA",
    };
    for (size_t i = 0; i < sizeof(logs) / sizeof(logs[0]); i++) {
        char log_cmd[40];
        snprintf(log_cmd, sizeof(log_cmd), "LOG COM%d %s ONTIME 1", BYNAV_COM_PORT, logs[i]);
        e = send_cmd(uart_num, log_cmd);
        if (e != ESP_OK) err = e;
    }

    e = send_cmd(uart_num, "SAVECONFIG");
    if (e != ESP_OK) err = e;

    return err;
}
