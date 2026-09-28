#include "i2c_shared_bus.h"

#include "esp_log.h"

static const char *TAG = "i2c_bus";

static i2c_master_bus_handle_t s_bus;
static int s_sda = -1;
static int s_scl = -1;

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
