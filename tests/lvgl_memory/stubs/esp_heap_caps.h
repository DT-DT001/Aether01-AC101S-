#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#define MALLOC_CAP_8BIT (1U << 2)
#define MALLOC_CAP_SPIRAM (1U << 10)
void *heap_caps_malloc(size_t size, uint32_t caps);
void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps);
void heap_caps_free(void *ptr);
bool heap_caps_check_integrity(uint32_t caps, bool print_errors);
