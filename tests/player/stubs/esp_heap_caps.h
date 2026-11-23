#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
#define MALLOC_CAP_INTERNAL 4
static inline void *heap_caps_malloc(size_t bytes, int caps) { (void)caps; return malloc(bytes); }
static inline void *heap_caps_realloc(void *ptr, size_t bytes, int caps) { (void)caps; return realloc(ptr, bytes); }
static inline void *heap_caps_calloc(size_t count, size_t bytes, int caps) { (void)caps; return calloc(count, bytes); }
