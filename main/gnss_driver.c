#include "gnss_driver.h"
#include "gnss_ubx.h"
#include "gnss_unicore.h"
#include "gnss_lc29h.h"

esp_err_t gnss_driver_configure(uart_port_t uart_num, gnss_chip_t chip, device_mode_t mode)
{
    if (chip == GNSS_CHIP_UBLOX) {
        return (mode == DEVICE_MODE_ROVER) ? gnss_ubx_configure_rover(uart_num)
                                            : gnss_ubx_configure_base(uart_num);
    }
    if (chip == GNSS_CHIP_UNICORE) {
        return (mode == DEVICE_MODE_ROVER) ? gnss_unicore_configure_rover(uart_num)
                                            : gnss_unicore_configure_base(uart_num);
    }
    if (chip == GNSS_CHIP_LC29H) {
        return (mode == DEVICE_MODE_ROVER) ? gnss_lc29h_configure_rover(uart_num)
                                            : gnss_lc29h_configure_base(uart_num);
    }
    return ESP_ERR_INVALID_ARG;
}
