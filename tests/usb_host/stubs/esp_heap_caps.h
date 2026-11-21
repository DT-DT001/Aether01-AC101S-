#pragma once
#include <stddef.h>
#define MALLOC_CAP_INTERNAL 1U
#define MALLOC_CAP_SPIRAM 2U
#define MALLOC_CAP_8BIT 4U
void *heap_caps_calloc(size_t,size_t,unsigned);
void heap_caps_free(void *);
