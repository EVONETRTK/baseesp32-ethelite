#include "i2c_shared_bus.h"

#include "esp_log.h"
#include "driver/gpio.h"
#include "esp_rom_sys.h"

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus;
static int s_sda = -1;
static int s_scl = -1;

// Sblocco del bus (procedura standard I2C): il ricevitore GNSS resta acceso
// mentre l'ESP32 si riavvia e puo' restare a meta' di un byte, tenendo SDA
// bassa; il driver I2C poi aspetterebbe il bus libero senza limite di tempo.
// Fino a 9 impulsi di clock finche' SDA torna alta, poi una condizione di STOP.
static void i2c_bus_clear(int sda_pin, int scl_pin)
{
    gpio_config_t io = {
        .pin_bit_mask = (1ULL << sda_pin) | (1ULL << scl_pin),
        .mode = GPIO_MODE_INPUT_OUTPUT_OD,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&io);
    gpio_set_level(sda_pin, 1);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(10);
    int sda0 = gpio_get_level(sda_pin), scl0 = gpio_get_level(scl_pin);
    int pulses = 0;
    while (gpio_get_level(sda_pin) == 0 && pulses < 9) {
        gpio_set_level(scl_pin, 0);
        esp_rom_delay_us(10);
        gpio_set_level(scl_pin, 1);
        esp_rom_delay_us(10);
        pulses++;
    }
    // STOP: SDA da bassa ad alta con SCL alta.
    gpio_set_level(sda_pin, 0);
    esp_rom_delay_us(10);
    gpio_set_level(scl_pin, 1);
    esp_rom_delay_us(10);
    gpio_set_level(sda_pin, 1);
    esp_rom_delay_us(10);
    if (sda0 == 0 || scl0 == 0) {
        ESP_LOGW(TAG, "Bus I2C occupato all'avvio (SDA=%d SCL=%d): sbloccato con %d impulsi, ora SDA=%d SCL=%d",
                 sda0, scl0, pulses, gpio_get_level(sda_pin), gpio_get_level(scl_pin));
    }
    gpio_reset_pin(sda_pin);
    gpio_reset_pin(scl_pin);
}

i2c_master_bus_handle_t i2c_shared_bus_get(int sda_pin, int scl_pin)
{
    if (sda_pin < 0 || scl_pin < 0) {
        return NULL;
    }
    if (s_bus) {
        if (sda_pin == s_sda && scl_pin == s_scl) {
            return s_bus;
        }
        ESP_LOGE(TAG, "Bus I2C gia' creato su SDA=%d SCL=%d, richiesto SDA=%d SCL=%d",
                 s_sda, s_scl, sda_pin, scl_pin);
        return NULL;
    }

    i2c_bus_clear(sda_pin, scl_pin);
    i2c_master_bus_config_t bus_cfg = {
        .i2c_port = -1,
        .sda_io_num = sda_pin,
        .scl_io_num = scl_pin,
        .clk_source = I2C_CLK_SRC_DEFAULT,
        .glitch_ignore_cnt = 7,
        .flags.enable_internal_pullup = true,
    };
    if (i2c_new_master_bus(&bus_cfg, &s_bus) != ESP_OK) {
        ESP_LOGE(TAG, "Init bus I2C fallita (SDA=%d SCL=%d)", sda_pin, scl_pin);
        s_bus = NULL;
        return NULL;
    }
    s_sda = sda_pin;
    s_scl = scl_pin;
    return s_bus;
}
