#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_8BIT 2U
#define MALLOC_CAP_DMA 4U
size_t heap_caps_get_free_size(unsigned);
size_t heap_caps_get_largest_free_block(unsigned);
