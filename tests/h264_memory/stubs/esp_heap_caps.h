#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1U << 2)
#define MALLOC_CAP_DMA (1U << 3)
#define MALLOC_CAP_SPIRAM (1U << 10)
#define MALLOC_CAP_INTERNAL (1U << 11)
#define MALLOC_CAP_CACHE_ALIGNED (1U << 19)
void *heap_caps_malloc_prefer(size_t size, size_t count, ...);
void *heap_caps_calloc_prefer(size_t n, size_t size, size_t count, ...);
