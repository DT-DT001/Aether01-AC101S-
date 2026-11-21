#pragma once
#include <stdlib.h>
#define MALLOC_CAP_SPIRAM 1
#define MALLOC_CAP_8BIT 2
extern int reader_test_fail_allocation;
static inline void *heap_caps_calloc(size_t count, size_t bytes, unsigned flags)
{
    (void)flags;
    if (reader_test_fail_allocation > 0 && --reader_test_fail_allocation == 0) return NULL;
    return calloc(count, bytes);
}
