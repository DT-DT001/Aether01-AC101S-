#pragma once
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_DMA 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_aligned_alloc(size_t, size_t, uint32_t);
