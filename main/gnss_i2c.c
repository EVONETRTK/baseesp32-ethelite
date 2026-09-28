#include "gnss_i2c.h"
#include "i2c_shared_bus.h"

#include "esp_log.h"

static const char *TAG = "gnss_i2c";

#define UBX_I2C_ADDR        0x42
// Registri dell'interfaccia DDC u-blox (Interface Description, sez. "DDC
// Port"): 0xFD/0xFE = numero di byte pronti (big-endian), 0xFF = stream dati.
#define UBX_I2C_REG_AVAIL   0xFD

bool gnss_i2c_probe(int sda_pin, int scl_pin)
{
    i2c_master_bus_handle_t bus = i2c_shared_bus_get(sda_pin, scl_pin);
    if (!bus) {
        ESP_LOGW(TAG, "Bus I2C non disponibile (SDA=%d SCL=%d)", sda_pin, scl_pin);
        return false;
    }
    if (i2c_master_probe(bus, UBX_I2C_ADDR, 100) != ESP_OK) {
        ESP_LOGW(TAG, "Nessun ricevitore u-blox all'indirizzo I2C 0x%02X (SDA=%d SCL=%d)",
                 UBX_I2C_ADDR, sda_pin, scl_pin);
        return false;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = UBX_I2C_ADDR,
        .scl_speed_hz = 100000,     // come gnss_io.c
        .scl_wait_us = 100000,
    };
    i2c_master_dev_handle_t dev;
    if (i2c_master_bus_add_device(bus, &dev_cfg, &dev) != ESP_OK) {
        ESP_LOGW(TAG, "Ricevitore trovato a 0x%02X ma aggiunta device I2C fallita", UBX_I2C_ADDR);
        return true;
    }
    uint8_t reg = UBX_I2C_REG_AVAIL;
    uint8_t avail[2] = {0};
    esp_err_t err = i2c_master_transmit_receive(dev, &reg, 1, avail, 2, 100);
    if (err == ESP_OK) {
        ESP_LOGI(TAG, "Ricevitore u-blox trovato via I2C a 0x%02X: %u byte pronti da leggere",
                 UBX_I2C_ADDR, (unsigned) ((avail[0] << 8) | avail[1]));
    } else {
        ESP_LOGW(TAG, "Ricevitore trovato a 0x%02X ma lettura registro 0xFD fallita: %s",
                 UBX_I2C_ADDR, esp_err_to_name(err));
    }
    i2c_master_bus_rm_device(dev);
    return true;
}
