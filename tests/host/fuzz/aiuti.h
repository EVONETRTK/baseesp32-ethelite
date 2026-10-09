// Aiuti comuni ai bersagli del fuzzing: ricalcolo dei controlli di
// integrita' (checksum NMEA, CRC24Q RTCM3, Fletcher UBX, CRC32 dei log ASCII
// Unicore) dopo le mutazioni, cosi' gli input arrivano anche oltre il
// controllo e mettono alla prova la lettura dei campi. Implementazioni
// scritte qui dagli standard, indipendenti dal firmware.
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <string.h>
#include <stdio.h>

static const char AIUTO_HEX[] = "0123456789ABCDEF";

// Riga NMEA "$....*HH" (stringa modificabile di capacita' cap): riscrive le
// due cifre dopo il primo '*', o aggiunge "*HH" in fondo se manca.
static void aiuto_nmea_checksum(char *s, size_t cap)
{
    if (s[0] != '$') return;
    unsigned char cs = 0;
    char *p = s + 1;
    for (; *p && *p != '*'; p++) cs ^= (unsigned char) *p;
    if (*p == '*') {
        if (p[1] && p[2]) {
            p[1] = AIUTO_HEX[cs >> 4];
            p[2] = AIUTO_HEX[cs & 15];
        } else if ((size_t) (p - s) + 4 <= cap) {
            p[1] = AIUTO_HEX[cs >> 4];
            p[2] = AIUTO_HEX[cs & 15];
            p[3] = '\0';
        }
    } else if ((size_t) (p - s) + 4 <= cap) {
        p[0] = '*';
        p[1] = AIUTO_HEX[cs >> 4];
        p[2] = AIUTO_HEX[cs & 15];
        p[3] = '\0';
    }
}

// CRC32 dei log ASCII in stile NovAtel (riflesso 0xEDB88320, iniziale 0).
static uint32_t aiuto_crc32_ascii(const char *s, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint8_t) s[i];
        for (int k = 0; k < 8; k++) crc = (crc & 1) ? (crc >> 1) ^ 0xEDB88320u : crc >> 1;
    }
    return crc;
}

// Riga "#....*xxxxxxxx": riscrive le 8 cifre dopo l'ultimo '*' (o le
// aggiunge se mancano e c'e' posto).
static void aiuto_unicore_crc(char *s, size_t cap)
{
    if (s[0] != '#') return;
    char *star = strrchr(s, '*');
    size_t len = strlen(s);
    if (!star) {
        if (len + 10 > cap) return;
        star = s + len;
        star[0] = '*';
        star[1] = '\0';
    }
    uint32_t c = aiuto_crc32_ascii(s + 1, (size_t) (star - s - 1));
    if ((size_t) (star - s) + 10 > cap) return;
    snprintf(star + 1, 9, "%08x", c);
}

static uint32_t aiuto_crc24q(const uint8_t *d, size_t n)
{
    uint32_t crc = 0;
    for (size_t i = 0; i < n; i++) {
        crc ^= (uint32_t) d[i] << 16;
        for (int k = 0; k < 8; k++) {
            crc <<= 1;
            if (crc & 0x1000000u) crc ^= 0x1864CFBu;
        }
    }
    return crc & 0xFFFFFFu;
}

static void aiuto_ubx_ck(const uint8_t *d, size_t n, uint8_t *a, uint8_t *b)
{
    uint8_t x = 0, y = 0;
    for (size_t i = 0; i < n; i++) {
        x += d[i];
        y += x;
    }
    *a = x;
    *b = y;
}

// Flusso binario misto: dove trova l'inizio di un frame RTCM3 (0xD3 + 6 bit
// a zero), UBX (B5 62) o una riga NMEA/ASCII che sta tutta nel buffer,
// ricalcola il controllo e salta oltre. Il resto resta com'e'.
static void aiuto_aggiusta_flusso(uint8_t *d, size_t n)
{
    size_t i = 0;
    while (i < n) {
        if (d[i] == 0xD3 && i + 3 <= n && !(d[i + 1] & 0xFC)) {
            size_t len = ((size_t) (d[i + 1] & 3) << 8) | d[i + 2];
            if (i + 3 + len + 3 <= n) {
                uint32_t c = aiuto_crc24q(d + i, 3 + len);
                d[i + 3 + len] = (uint8_t) (c >> 16);
                d[i + 3 + len + 1] = (uint8_t) (c >> 8);
                d[i + 3 + len + 2] = (uint8_t) c;
                i += 3 + len + 3;
                continue;
            }
        } else if (d[i] == 0xB5 && i + 6 <= n && d[i + 1] == 0x62) {
            size_t len = d[i + 4] | ((size_t) d[i + 5] << 8);
            if (i + 6 + len + 2 <= n) {
                aiuto_ubx_ck(d + i + 2, 4 + len, &d[i + 6 + len], &d[i + 6 + len + 1]);
                i += 8 + len;
                continue;
            }
        } else if (d[i] == '$' || d[i] == '#') {
            size_t j = i + 1;
            while (j < n && d[j] != '*' && d[j] != '\r' && d[j] != '\n') j++;
            size_t cifre = d[i] == '$' ? 2 : 8;
            if (j < n && d[j] == '*' && j + cifre < n) {
                if (d[i] == '$') {
                    unsigned char cs = 0;
                    for (size_t k = i + 1; k < j; k++) cs ^= d[k];
                    d[j + 1] = (uint8_t) AIUTO_HEX[cs >> 4];
                    d[j + 2] = (uint8_t) AIUTO_HEX[cs & 15];
                } else {
                    char tmp[9];
                    snprintf(tmp, sizeof(tmp), "%08x", aiuto_crc32_ascii((const char *) d + i + 1, j - i - 1));
                    memcpy(d + j + 1, tmp, 8);
                }
                i = j + 1 + cifre;
                continue;
            }
        }
        i++;
    }
}

// Copia modificabile (statica, fino a 64 KB) di un input.
static uint8_t *aiuto_copia(const uint8_t *d, size_t n)
{
    static uint8_t buf[65536 + 16];
    if (n > 65536) n = 65536;
    memcpy(buf, d, n);
    buf[n] = 0;
    return buf;
}
