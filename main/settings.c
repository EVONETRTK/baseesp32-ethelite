#include "settings.h"

#include <string.h>
#include <stdio.h>

#include "nvs.h"
#include "esp_mac.h"
#include "esp_log.h"

#include "sdkconfig.h"

static const char *TAG = "settings";

#define NVS_NAMESPACE "baseesp32"
#define NVS_KEY_CFG   "cfg"
#define CFG_MAGIC     0x62733031u // "bs01"

typedef struct {
    uint32_t magic;
    app_settings_t s;
} stored_cfg_t;

static app_settings_t s_settings;

static void apply_defaults(void)
{
    memset(&s_settings, 0, sizeof(s_settings));

    strncpy(s_settings.wifi_ssid, CONFIG_BASEESP32_WIFI_SSID, sizeof(s_settings.wifi_ssid) - 1);
    strncpy(s_settings.wifi_password, CONFIG_BASEESP32_WIFI_PASSWORD, sizeof(s_settings.wifi_password) - 1);
#if CONFIG_BASEESP32_CELLULAR_ENABLE
    strncpy(s_settings.cellular_apn, CONFIG_BASEESP32_CELLULAR_APN, sizeof(s_settings.cellular_apn) - 1);
#endif
#ifdef CONFIG_BASEESP32_CELLULAR_MODEM_IS_SIM868
    s_settings.cellular_is_sim868 = true;
#else
    s_settings.cellular_is_sim868 = false;
#endif
    strncpy(s_settings.ntrip_host, CONFIG_BASEESP32_NTRIP_HOST, sizeof(s_settings.ntrip_host) - 1);
    s_settings.ntrip_port = CONFIG_BASEESP32_NTRIP_PORT;
    strncpy(s_settings.ntrip_mountpoint, CONFIG_BASEESP32_NTRIP_MOUNTPOINT, sizeof(s_settings.ntrip_mountpoint) - 1);
    s_settings.gnss_chip = GNSS_CHIP_UBLOX;
    s_settings.device_mode = DEVICE_MODE_BASE;
    s_settings.network_mode = NETWORK_MODE_BOTH;
    s_settings.nmea_udp_port = 5005;

    s_settings.gnss_uart_num = CONFIG_BASEESP32_GNSS_UART_NUM;
    s_settings.gnss_uart_tx_pin = CONFIG_BASEESP32_GNSS_UART_TX_PIN;
    s_settings.gnss_uart_rx_pin = CONFIG_BASEESP32_GNSS_UART_RX_PIN;
    s_settings.gnss_uart_baud = CONFIG_BASEESP32_GNSS_UART_BAUD;

    s_settings.rgb_led_mode = RGB_LED_NONE;
    s_settings.rgb_led_ws2812_pin = CONFIG_BASEESP32_RGB_WS2812_PIN;
    s_settings.rgb_led_pwm_r_pin = CONFIG_BASEESP32_RGB_PWM_R_PIN;
    s_settings.rgb_led_pwm_g_pin = CONFIG_BASEESP32_RGB_PWM_G_PIN;
    s_settings.rgb_led_pwm_b_pin = CONFIG_BASEESP32_RGB_PWM_B_PIN;
    s_settings.rgb_led_pwm_active_low = false;

    s_settings.oled_sda_pin = CONFIG_BASEESP32_OLED_SDA_PIN;
    s_settings.oled_scl_pin = CONFIG_BASEESP32_OLED_SCL_PIN;
    s_settings.oled_i2c_addr = CONFIG_BASEESP32_OLED_I2C_ADDR;
#ifdef CONFIG_BASEESP32_OLED_CONTROLLER_SH1106
    s_settings.oled_is_sh1106 = true;
#else
    s_settings.oled_is_sh1106 = false;
#endif
#ifdef CONFIG_BASEESP32_OLED_FLIP_H
    s_settings.oled_flip_h = true;
#else
    s_settings.oled_flip_h = false;
#endif
#ifdef CONFIG_BASEESP32_OLED_FLIP_V
    s_settings.oled_flip_v = true;
#else
    s_settings.oled_flip_v = false;
#endif

    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_WIFI_STA);
    snprintf(s_settings.ap_ssid, sizeof(s_settings.ap_ssid), "baseesp32-%02X%02X%02X", mac[3], mac[4], mac[5]);
    strncpy(s_settings.ap_password, "baseesp32setup", sizeof(s_settings.ap_password) - 1);
    strncpy(s_settings.admin_code, "1234", sizeof(s_settings.admin_code) - 1);
}

void settings_init(void)
{
    apply_defaults();

    nvs_handle_t h;
    if (nvs_open(NVS_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        ESP_LOGW(TAG, "Nessuna configurazione salvata in NVS, uso i default di Kconfig");
        return;
    }

    stored_cfg_t stored;
    size_t len = sizeof(stored);
    esp_err_t err = nvs_get_blob(h, NVS_KEY_CFG, &stored, &len);
    nvs_close(h);

    if (err == ESP_OK && len == sizeof(stored) && stored.magic == CFG_MAGIC) {
        s_settings = stored.s;
        ESP_LOGI(TAG, "Configurazione caricata da NVS (AP=%s)", s_settings.ap_ssid);
    } else {
        ESP_LOGW(TAG, "Configurazione NVS assente/non valida, uso i default di Kconfig");
    }
}

app_settings_t settings_get(void)
{
    return s_settings;
}

esp_err_t settings_save(const app_settings_t *s)
{
    s_settings = *s;

    stored_cfg_t stored = { .magic = CFG_MAGIC, .s = s_settings };

    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &h);
    if (err != ESP_OK) {
        return err;
    }

    err = nvs_set_blob(h, NVS_KEY_CFG, &stored, sizeof(stored));
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }
    nvs_close(h);
    return err;
}
