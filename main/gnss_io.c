#include "gnss_io.h"
#include "i2c_shared_bus.h"
#include "sd_mutex.h"

#include <string.h>

#include "driver/uart.h"
#include "driver/i2c_master.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/task.h"
#include "freertos/stream_buffer.h"

static const char *TAG = "gnss_io";

#define UART_RX_BUF_SIZE 1024

// Interfaccia DDC (I2C) u-blox: indirizzo 0x42, registri 0xFD/0xFE = byte
// pronti (big-endian), 0xFF = stream dati. Vedi u-blox Interface Description,
// sezione "DDC Port".
#define UBX_I2C_ADDR        0x42
#define UBX_I2C_REG_AVAIL   0xFD
#define UBX_I2C_REG_STREAM  0xFF
#define I2C_POLL_IDLE_MS    20
#define I2C_READ_CHUNK      256
#define I2C_WRITE_CHUNK     256
// 100 kHz invece di 400: piu' tollerante con cavi/pull-up dell'HAT, e
// comunque ~10 KB/s, molto piu' del flusso RTCM3 di una base (1-2 KB/s).
#define I2C_SPEED_HZ        100000
// Il ricevitore tiene basso il clock (clock stretching) mentre elabora un
// comando: con l'attesa di default l'ESP32 lo scambia per un errore. Valore
// alto apposta, il driver lo limita da solo al massimo supportato.
#define I2C_SCL_WAIT_US     100000

static bool s_i2c;
static uart_port_t s_uart_num;

static i2c_master_bus_handle_t s_bus;
static i2c_master_dev_handle_t s_dev;
static StreamBufferHandle_t s_i2c_rx;
static StreamBufferHandle_t s_i2c_tx;

// Serie di errori consecutivi, per un riepilogo nel log quando il bus torna
// a funzionare (quanti errori e per quanto tempo): serve a capire con cosa
// coincidono (es. scritture sulla SD, attivita' WiFi all'avvio).
static int s_err_streak;
static int64_t s_err_streak_start_us;

static void i2c_recover(const char *what, esp_err_t err)
{
    if (s_err_streak++ == 0) {
        s_err_streak_start_us = esp_timer_get_time();
    }
    ESP_LOGW(TAG, "I2C %s fallita (%s): reset del bus", what, esp_err_to_name(err));
    i2c_master_bus_reset(s_bus);
    vTaskDelay(pdMS_TO_TICKS(50));
}

static void i2c_note_ok(void)
{
    if (s_err_streak > 0) {
        ESP_LOGI(TAG, "I2C di nuovo funzionante dopo %d errori in %lld ms", s_err_streak,
                 (esp_timer_get_time() - s_err_streak_start_us) / 1000);
        s_err_streak = 0;
    }
}

// Unico task che parla con il ricevitore via I2C: manda quello che gli
// altri task hanno messo in coda con gnss_io_write(), poi legge quello che
// il ricevitore ha pronto e lo mette nello stream letto da gnss_io_read()
// (con I2C e' il master a dover chiedere i dati, il ricevitore non li
// "spinge" come su seriale). Tutto da un solo task: niente scritture e
// letture intrecciate da task diversi.
static void i2c_poll_task(void *arg)
{
    uint8_t buf[I2C_READ_CHUNK > I2C_WRITE_CHUNK ? I2C_READ_CHUNK : I2C_WRITE_CHUNK];
    // u-blox interpreta una scrittura I2C di un solo byte come "imposta il
    // registro da leggere", non come dato: un byte isolato (es. l'ultimo di
    // un pacchetto RTCM dal caster) resta qui e parte in testa al prossimo
    // invio.
    int pending = -1;

    while (1) {
        // Mai I2C mentre la microSD e' montata: sulla base di prova il clock
        // della SD (GPIO10) e' collegato a una linea I2C tramite l'HAT, e le
        // due cose insieme bloccano il bus (verificato pilotando un pin alla
        // volta, vedi CHANGELOG 1.19.70). Il lucchetto e' quello gia' usato da
        // tutti i moduli che montano la SD; durante le brevi pause il
        // ricevitore tiene i dati nel suo buffer.
        if (!sd_mutex_try_take(0)) {
            vTaskDelay(pdMS_TO_TICKS(I2C_POLL_IDLE_MS));
            continue;
        }
        bool did_work = false;

        size_t off = 0;
        if (pending >= 0) {
            buf[off++] = (uint8_t) pending;
            pending = -1;
        }
        off += xStreamBufferReceive(s_i2c_tx, buf + off, I2C_WRITE_CHUNK - off, 0);
        if (off == 1) {
            pending = buf[0];
        } else if (off > 1) {
            // Un secondo tentativo immediato prima di dare errore: un disturbo
            // isolato sul bus (visto sul dispositivo a ogni montaggio della
            // microSD, con i soli pull-up interni dell'ESP32) faceva perdere
            // la scrittura, cioe' un comando o un pezzo di RTCM verso il
            // ricevitore.
            esp_err_t err = i2c_master_transmit(s_dev, buf, off, 500);
            if (err != ESP_OK) {
                i2c_master_bus_reset(s_bus);
                err = i2c_master_transmit(s_dev, buf, off, 500);
            }
            if (err != ESP_OK) {
                i2c_recover("scrittura", err);
            }
            did_work = true;
        }

        uint8_t reg = UBX_I2C_REG_AVAIL;
        uint8_t avail_be[2];
        esp_err_t err = i2c_master_transmit_receive(s_dev, &reg, 1, avail_be, 2, 100);
        if (err != ESP_OK) {
            // Come sopra: si ripete una volta prima di considerarlo un errore.
            i2c_master_bus_reset(s_bus);
            err = i2c_master_transmit_receive(s_dev, &reg, 1, avail_be, 2, 100);
        }
        if (err != ESP_OK) {
            i2c_recover("lettura byte pronti", err); // col lucchetto ancora preso: il reset del bus agita le linee I2C
            sd_mutex_give();
            continue;
        }
        i2c_note_ok();
        uint16_t avail = (uint16_t) ((avail_be[0] << 8) | avail_be[1]);
        if (avail != 0 && avail != 0xFFFF) {
            size_t n = avail < I2C_READ_CHUNK ? avail : I2C_READ_CHUNK;
            reg = UBX_I2C_REG_STREAM;
            err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 400);
            if (err != ESP_OK) {
                // Stesso secondo tentativo delle altre transazioni (visto un
                // timeout qui durante lo smontaggio della microSD). Se il
                // primo tentativo aveva gia' letto una parte dei byte, quei
                // byte sono persi: il frame RTCM interessato verra' scartato
                // dal controllo CRC, i successivi arrivano normalmente.
                i2c_master_bus_reset(s_bus);
                err = i2c_master_transmit_receive(s_dev, &reg, 1, buf, n, 400);
            }
            if (err == ESP_OK) {
                xStreamBufferSend(s_i2c_rx, buf, n, pdMS_TO_TICKS(100));
            } else {
                i2c_recover("lettura dati", err);
            }
            did_work = true;
        }

        sd_mutex_give();
        if (!did_work) {
            vTaskDelay(pdMS_TO_TICKS(I2C_POLL_IDLE_MS));
        }
    }
}

static bool i2c_init(const app_settings_t *settings)
{
    s_bus = i2c_shared_bus_get(settings->oled_sda_pin, settings->oled_scl_pin);
    if (!s_bus) {
        ESP_LOGE(TAG, "GNSS via I2C: bus non disponibile (SDA=%d SCL=%d, pin del riquadro OLED)",
                 settings->oled_sda_pin, settings->oled_scl_pin);
        return false;
    }
    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = UBX_I2C_ADDR,
        .scl_speed_hz = I2C_SPEED_HZ,
        .scl_wait_us = I2C_SCL_WAIT_US,
    };
    if (i2c_master_bus_add_device(s_bus, &dev_cfg, &s_dev) != ESP_OK) {
        ESP_LOGE(TAG, "GNSS via I2C: aggiunta device 0x%02X fallita", UBX_I2C_ADDR);
        return false;
    }
    s_i2c_rx = xStreamBufferCreate(4096, 1);
    s_i2c_tx = xStreamBufferCreate(4096, 1);
    // Core 1: se il driver I2C dovesse mai restare in attesa del bus, non
    // blocca il core 0 (WiFi, rete, pannello web).
    xTaskCreatePinnedToCore(i2c_poll_task, "gnss_i2c_poll", 3072, NULL, 5, NULL, 1);
    ESP_LOGI(TAG, "Ricevitore GNSS via I2C (u-blox 0x%02X, SDA=%d SCL=%d, %d kHz)",
             UBX_I2C_ADDR, settings->oled_sda_pin, settings->oled_scl_pin, I2C_SPEED_HZ / 1000);
    return true;
}

static void uart_init(const app_settings_t *settings)
{
    s_uart_num = (uart_port_t) settings->gnss_uart_num;

    uart_config_t uart_config = {
        .baud_rate = settings->gnss_uart_baud,
        .data_bits = UART_DATA_8_BITS,
        .parity    = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    ESP_ERROR_CHECK(uart_driver_install(s_uart_num, UART_RX_BUF_SIZE * 2, UART_RX_BUF_SIZE * 2, 0, NULL, 0));
    ESP_ERROR_CHECK(uart_param_config(s_uart_num, &uart_config));
    ESP_ERROR_CHECK(uart_set_pin(s_uart_num, settings->gnss_uart_tx_pin, settings->gnss_uart_rx_pin,
                                  UART_PIN_NO_CHANGE, UART_PIN_NO_CHANGE));
}

void gnss_io_init(const app_settings_t *settings)
{
    if (settings->gnss_i2c) {
        if (settings->gnss_chip != GNSS_CHIP_UBLOX) {
            ESP_LOGW(TAG, "Collegamento I2C scelto ma il chip impostato non e' u-blox: l'I2C 0x42 e' solo u-blox");
        }
        s_i2c = i2c_init(settings);
        if (s_i2c) {
            return;
        }
        ESP_LOGW(TAG, "I2C non disponibile, uso la seriale");
    }
    uart_init(settings);
}

bool gnss_io_is_i2c(void)
{
    return s_i2c;
}

int gnss_io_read(uint8_t *buf, size_t len, TickType_t timeout)
{
    if (s_i2c) {
        return (int) xStreamBufferReceive(s_i2c_rx, buf, len, timeout);
    }
    return uart_read_bytes(s_uart_num, buf, len, timeout);
}

int gnss_io_write(const void *data, size_t len)
{
    if (s_i2c) {
        // In coda: la scrittura vera la fa i2c_poll_task, entro ~20 ms.
        return (int) xStreamBufferSend(s_i2c_tx, data, len, pdMS_TO_TICKS(500));
    }
    return uart_write_bytes(s_uart_num, data, len);
}
