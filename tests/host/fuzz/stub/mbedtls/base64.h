// Stub di mbedtls/base64.h per il fuzzing sul PC: implementazione in
// fuzz_ntrip.c con lo stesso comportamento di mbedTLS (buffer piccolo =
// errore, *olen = spazio necessario).
#pragma once
#include <stddef.h>
#define MBEDTLS_ERR_BASE64_BUFFER_TOO_SMALL -0x002A
int mbedtls_base64_encode(unsigned char *dst, size_t dlen, size_t *olen, const unsigned char *src, size_t slen);
