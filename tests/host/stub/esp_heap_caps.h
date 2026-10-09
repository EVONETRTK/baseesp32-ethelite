// Stub di esp_heap_caps.h: sul PC tutto con malloc/calloc.
#pragma once
#include <stdlib.h>
#include <stdint.h>
#define MALLOC_CAP_SPIRAM   (1 << 10)
#define MALLOC_CAP_INTERNAL (1 << 11)
#define MALLOC_CAP_8BIT     (1 << 2)
#define MALLOC_CAP_DEFAULT  (1 << 12)
#define heap_caps_malloc(size, caps) malloc(size)
#define heap_caps_calloc(n, size, caps) calloc((n), (size))
#define heap_caps_free(p) free(p)
