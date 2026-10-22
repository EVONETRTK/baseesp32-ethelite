#pragma once

#include <stdint.h>
#include <stddef.h>

// CRC24Q dei frame RTCM3 (polinomio 0x1864CFB, non riflesso, valore
// iniziale 0) come da standard RTCM 10403.x, calcolato sui primi n byte di
// buf (intestazione + payload, esclusi i 3 byte del CRC). Unico calcolo
// usato da rtcm3_1005.c, base_stream_demux.c e base_selfpos.c (parola
// SBAS/Galileo). Valore di controllo: "123456789" -> 0xCDE703.
uint32_t rtcm3_crc24q(const uint8_t *buf, size_t n);
