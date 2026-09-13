#include "rgb_led.h"
#include "settings.h"

#include <stdbool.h>
#include "esp_log.h"
#include "led_strip.h"
#include "driver/ledc.h"

static const char *TAG = "rgb_led";
static rgb_led_mode_t s_mode = RGB_LED_NONE;

// --- Backend WS2812/NeoPixel (un pin dati, via RMT) ---
static led_strip_handle_t s_strip = NULL;

static void ws2812_init(int pin)
{
    led_strip_config_t strip_config = {
        .strip_gpio_num = pin,
        .max_leds = 1,
        .led_pixel_format = LED_PIXEL_FORMAT_GRB,
        .led_model = LED_MODEL_WS2812,
    };
    led_strip_rmt_config_t rmt_config = {
        .clk_src = RMT_CLK_SRC_DEFAULT,
        .resolution_hz = 10 * 1000 * 1000,
    };
    if (led_strip_new_rmt_device(&strip_config, &rmt_config, &s_strip) != ESP_OK) {
        ESP_LOGE(TAG, "Init LED WS2812 fallita (pin %d)", pin);
        s_strip = NULL;
    }
}

static void ws2812_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_strip) {
        return;
    }
    led_strip_set_pixel(s_strip, 0, r, g, b);
    led_strip_refresh(s_strip);
}

// --- Backend RGB a 3 pin separati, intensita' via PWM (LEDC) ---
static bool s_pwm_ok = false;
static bool s_pwm_active_low = false;

static void pwm3_init(int r_pin, int g_pin, int b_pin, bool active_low)
{
    s_pwm_active_low = active_low;

    ledc_timer_config_t timer_conf = {
        .speed_mode = LEDC_LOW_SPEED_MODE,
        .duty_resolution = LEDC_TIMER_8_BIT,
        .timer_num = LEDC_TIMER_0,
        .freq_hz = 5000,
        .clk_cfg = LEDC_AUTO_CLK,
    };
    if (ledc_timer_config(&timer_conf) != ESP_OK) {
        ESP_LOGE(TAG, "Init timer LEDC per RGB PWM fallita");
        return;
    }

    int pins[3] = { r_pin, g_pin, b_pin };
    for (int i = 0; i < 3; i++) {
        ledc_channel_config_t ch_conf = {
            .gpio_num = pins[i],
            .speed_mode = LEDC_LOW_SPEED_MODE,
            .channel = (ledc_channel_t) i,
            .timer_sel = LEDC_TIMER_0,
            .duty = s_pwm_active_low ? 255 : 0,
            .hpoint = 0,
        };
        if (ledc_channel_config(&ch_conf) != ESP_OK) {
            ESP_LOGE(TAG, "Init canale LEDC %d (pin %d) per RGB PWM fallita", i, pins[i]);
            return;
        }
    }
    s_pwm_ok = true;
}

static void pwm3_set(uint8_t r, uint8_t g, uint8_t b)
{
    if (!s_pwm_ok) {
        return;
    }
    uint8_t vals[3] = { r, g, b };
    for (int i = 0; i < 3; i++) {
        uint32_t duty = s_pwm_active_low ? (255 - vals[i]) : vals[i];
        ledc_set_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t) i, duty);
        ledc_update_duty(LEDC_LOW_SPEED_MODE, (ledc_channel_t) i);
    }
}

void rgb_led_init(void)
{
    app_settings_t settings = settings_get();
    s_mode = settings.rgb_led_mode;

    if (s_mode == RGB_LED_WS2812 && settings.rgb_led_ws2812_pin >= 0) {
        ws2812_init(settings.rgb_led_ws2812_pin);
    } else if (s_mode == RGB_LED_PWM3 &&
               settings.rgb_led_pwm_r_pin >= 0 &&
               settings.rgb_led_pwm_g_pin >= 0 &&
               settings.rgb_led_pwm_b_pin >= 0) {
        pwm3_init(settings.rgb_led_pwm_r_pin, settings.rgb_led_pwm_g_pin,
                  settings.rgb_led_pwm_b_pin, settings.rgb_led_pwm_active_low);
    } else {
        s_mode = RGB_LED_NONE;
    }
}

void rgb_led_set(uint8_t r, uint8_t g, uint8_t b)
{
    switch (s_mode) {
    case RGB_LED_WS2812:
        ws2812_set(r, g, b);
        break;
    case RGB_LED_PWM3:
        pwm3_set(r, g, b);
        break;
    default:
        break;
    }
}
