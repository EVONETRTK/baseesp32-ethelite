#include "rtcm3_crc.h"

// Versione bit-a-bit (senza tabella): il volume di dati da un singolo
// ricevitore base e' basso, non serve ottimizzare la velocita' a scapito
// della memoria flash di una tabella.
uint32_t rtcm3_crc24q(const uint8_t *buf, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= ((uint32_t) buf[i]) << 16;
        for (int j = 0; j < 8; j++) {
            crc <<= 1;
            if (crc & 0x01000000) {
                crc ^= 0x01864CFB;
            }
        }
    }
    return crc & 0x00FFFFFF;
}
