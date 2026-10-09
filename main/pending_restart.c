#include "pending_restart.h"

#include <stdbool.h>
#include <string.h>

// Solo i campi che servono al confronto (pochi byte, non una seconda copia
// intera di app_settings_t).
typedef struct {
    device_mode_t device_mode;
    gnss_chip_t gnss_chip;
    bool gnss_i2c;
    int gnss_uart_num, gnss_uart_tx_pin, gnss_uart_rx_pin, gnss_uart_baud;
    base_position_mode_t base_position_mode;
    double base_fixed_lat_deg, base_fixed_lon_deg, base_fixed_height_m;
    uint16_t base_svin_min_dur_s;
    float base_svin_acc_m;
    bool ntrip_caster_server_enable;
    uint16_t ntrip_caster_server_port;
    bool cellular_is_sim868, cellular_simcom_std;
    char cellular_apn[64];
    bool comnav_ins_enable, comnav_heading_enable;
    uint8_t comnav_imu_axes;
    bool rover_has_fallback;
    network_mode_t network_mode;
    uint8_t nmea_rate_hz;
    int oled_sda_pin, oled_scl_pin;
    uint8_t oled_i2c_addr;
    oled_controller_t oled_controller;
    bool oled_flip_h, oled_flip_v;
    rgb_led_mode_t rgb_led_mode;
    int rgb_led_ws2812_pin, rgb_led_pwm_r_pin, rgb_led_pwm_g_pin, rgb_led_pwm_b_pin;
    bool rgb_led_pwm_active_low;
} restart_fields_t;

static restart_fields_t s_boot;
static bool s_have;

static void pick(const app_settings_t *s, restart_fields_t *o)
{
    memset(o, 0, sizeof(*o));
    o->device_mode = s->device_mode;
    o->gnss_chip = s->gnss_chip;
    o->gnss_i2c = s->gnss_i2c;
    o->gnss_uart_num = s->gnss_uart_num;
    o->gnss_uart_tx_pin = s->gnss_uart_tx_pin;
    o->gnss_uart_rx_pin = s->gnss_uart_rx_pin;
    o->gnss_uart_baud = s->gnss_uart_baud;
    o->base_position_mode = s->base_position_mode;
    o->base_fixed_lat_deg = s->base_fixed_lat_deg;
    o->base_fixed_lon_deg = s->base_fixed_lon_deg;
    o->base_fixed_height_m = s->base_fixed_height_m;
    o->base_svin_min_dur_s = s->base_svin_min_dur_s;
    o->base_svin_acc_m = s->base_svin_acc_m;
    o->ntrip_caster_server_enable = s->ntrip_caster_server_enable;
    o->ntrip_caster_server_port = s->ntrip_caster_server_port;
    o->cellular_is_sim868 = s->cellular_is_sim868;
    o->cellular_simcom_std = s->cellular_simcom_std;
    strlcpy(o->cellular_apn, s->cellular_apn, sizeof(o->cellular_apn));
    o->comnav_ins_enable = s->comnav_ins_enable;
    o->comnav_heading_enable = s->comnav_heading_enable;
    o->comnav_imu_axes = s->comnav_imu_axes;
    o->rover_has_fallback = s->rover_has_fallback;
    o->network_mode = s->network_mode;
    o->nmea_rate_hz = s->nmea_rate_hz;
    o->oled_sda_pin = s->oled_sda_pin;
    o->oled_scl_pin = s->oled_scl_pin;
    o->oled_i2c_addr = s->oled_i2c_addr;
    o->oled_controller = s->oled_controller;
    o->oled_flip_h = s->oled_flip_h;
    o->oled_flip_v = s->oled_flip_v;
    o->rgb_led_mode = s->rgb_led_mode;
    o->rgb_led_ws2812_pin = s->rgb_led_ws2812_pin;
    o->rgb_led_pwm_r_pin = s->rgb_led_pwm_r_pin;
    o->rgb_led_pwm_g_pin = s->rgb_led_pwm_g_pin;
    o->rgb_led_pwm_b_pin = s->rgb_led_pwm_b_pin;
    o->rgb_led_pwm_active_low = s->rgb_led_pwm_active_low;
}

void pending_restart_snapshot(const app_settings_t *s)
{
    pick(s, &s_boot);
    s_have = true;
}

static void add(char *out, size_t out_size, const char *what)
{
    if (out[0]) {
        strlcat(out, ", ", out_size);
    }
    strlcat(out, what, out_size);
}

void pending_restart_describe(const app_settings_t *s, char *out, size_t out_size)
{
    out[0] = '\0';
    if (!s_have) {
        return;
    }
    restart_fields_t n;
    pick(s, &n);
    const restart_fields_t *b = &s_boot;
    if (n.device_mode != b->device_mode) add(out, out_size, "funzione base/rover");
    if (n.network_mode != b->network_mode) add(out, out_size, "connettivita'");
    if (n.gnss_chip != b->gnss_chip) add(out, out_size, "ricevitore GNSS");
    if (n.gnss_i2c != b->gnss_i2c || n.gnss_uart_num != b->gnss_uart_num || n.gnss_uart_tx_pin != b->gnss_uart_tx_pin ||
        n.gnss_uart_rx_pin != b->gnss_uart_rx_pin || n.gnss_uart_baud != b->gnss_uart_baud) {
        add(out, out_size, "collegamento del ricevitore");
    }
    if (n.base_position_mode != b->base_position_mode || n.base_fixed_lat_deg != b->base_fixed_lat_deg ||
        n.base_fixed_lon_deg != b->base_fixed_lon_deg || n.base_fixed_height_m != b->base_fixed_height_m ||
        n.base_svin_min_dur_s != b->base_svin_min_dur_s || n.base_svin_acc_m != b->base_svin_acc_m) {
        add(out, out_size, "posizione della base");
    }
    if (n.ntrip_caster_server_enable != b->ntrip_caster_server_enable ||
        n.ntrip_caster_server_port != b->ntrip_caster_server_port) {
        add(out, out_size, "caster locale");
    }
    if (n.cellular_is_sim868 != b->cellular_is_sim868 || n.cellular_simcom_std != b->cellular_simcom_std ||
        strcmp(n.cellular_apn, b->cellular_apn) != 0) {
        add(out, out_size, "modem e APN");
    }
    if (n.comnav_ins_enable != b->comnav_ins_enable || n.comnav_heading_enable != b->comnav_heading_enable ||
        n.comnav_imu_axes != b->comnav_imu_axes) {
        add(out, out_size, "IMU e doppia antenna (K922)");
    }
    if (n.rover_has_fallback != b->rover_has_fallback) add(out, out_size, "riserva Galileo HAS");
    if (n.nmea_rate_hz != b->nmea_rate_hz) add(out, out_size, "posizioni al secondo");
    if (n.oled_sda_pin != b->oled_sda_pin || n.oled_scl_pin != b->oled_scl_pin || n.oled_i2c_addr != b->oled_i2c_addr ||
        n.oled_controller != b->oled_controller || n.oled_flip_h != b->oled_flip_h || n.oled_flip_v != b->oled_flip_v) {
        add(out, out_size, "display OLED");
    }
    if (n.rgb_led_mode != b->rgb_led_mode || n.rgb_led_ws2812_pin != b->rgb_led_ws2812_pin ||
        n.rgb_led_pwm_r_pin != b->rgb_led_pwm_r_pin || n.rgb_led_pwm_g_pin != b->rgb_led_pwm_g_pin ||
        n.rgb_led_pwm_b_pin != b->rgb_led_pwm_b_pin || n.rgb_led_pwm_active_low != b->rgb_led_pwm_active_low) {
        add(out, out_size, "LED");
    }
}
