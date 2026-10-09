// Incluso per primo in ogni file compilato sul PC (opzione -include di
// run.ps1): funzioni di newlib/ESP-IDF che la libreria C di Windows (mingw)
// non ha. Solo per le prove sul PC, mai nel firmware.
#pragma once

#include <string.h>
#include <time.h>
#include <math.h>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif

static inline size_t host_strlcpy(char *dst, const char *src, size_t size)
{
    size_t n = strlen(src);
    if (size) {
        size_t k = n < size - 1 ? n : size - 1;
        memcpy(dst, src, k);
        dst[k] = '\0';
    }
    return n;
}
#define strlcpy host_strlcpy

static inline struct tm *host_gmtime_r(const time_t *t, struct tm *out)
{
    return gmtime_s(out, t) == 0 ? out : NULL;
}
#define gmtime_r host_gmtime_r
