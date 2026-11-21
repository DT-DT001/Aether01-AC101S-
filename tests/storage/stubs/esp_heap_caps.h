#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1
#define MALLOC_CAP_DMA 2
#define MALLOC_CAP_8BIT 4
void *heap_caps_aligned_alloc(size_t alignment, size_t size, unsigned caps);
