#include "gnss_driver.h"
#include "gnss_ubx.h"
#include "gnss_unicore.h"
#include "gnss_lc29h.h"
#include "gnss_bynav.h"
#include "gnss_l76k.h"
#include "gnss_comnav.h"

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
    if (chip == GNSS_CHIP_BYNAV) {
        return (mode == DEVICE_MODE_ROVER) ? gnss_bynav_configure_rover(uart_num)
                                            : gnss_bynav_configure_base(uart_num);
    }
    if (chip == GNSS_CHIP_BYNAV_M21D) {
        // Base: identica a M20D (survey-in + RTCM3, vedi sopra) - una base
        // e' ferma, l'IMU/INS non le serve. Solo il rover usa la
        // configurazione INS specifica di M21D (lever arm, RBV, log prua/
        // assetto), vedi gnss_bynav_m21d_configure_rover().
        return (mode == DEVICE_MODE_ROVER) ? gnss_bynav_m21d_configure_rover(uart_num)
                                            : gnss_bynav_configure_base(uart_num);
    }
    if (chip == GNSS_CHIP_COMNAV) {
        return (mode == DEVICE_MODE_ROVER) ? gnss_comnav_configure_rover(uart_num)
                                            : gnss_comnav_configure_base(uart_num);
    }
    if (chip == GNSS_CHIP_L76K) {
        return (mode == DEVICE_MODE_ROVER) ? gnss_l76k_configure_rover(uart_num)
                                            : gnss_l76k_configure_base(uart_num);
    }
    return ESP_ERR_INVALID_ARG;
}
