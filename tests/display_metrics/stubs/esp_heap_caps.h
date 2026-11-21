#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_DMA 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_aligned_alloc(size_t alignment,size_t size,uint32_t caps);
extern bool test_heap_fail;
extern unsigned test_heap_live;
