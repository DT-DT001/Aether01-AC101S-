#include "lvgl.h"
#include "src/core/lv_global.h"
#include "esp_heap_caps.h"
#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Supply the real LVGL global type without starting display/OS services. */
lv_global_t lv_global = {.memory_zero = ZERO_MEM_SENTINEL};
enum { UI_CAPS = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT, RECORDS = 2048 };
static struct { void *ptr; size_t size; } records[RECORDS];
static size_t live, malloc_calls, realloc_calls, free_calls, integrity_calls;
static bool fail_next, integrity_ok = true;

static size_t find(void *ptr)
{
    for(size_t i = 0; i < RECORDS; ++i) if(records[i].ptr == ptr) return i;
    assert(!"Unknown/freed pointer or LVGL zero sentinel reached IDF allocator");
    return 0;
}

static void remember(void *ptr, size_t size)
{
    assert(ptr && (uintptr_t)ptr % 4 == 0);
    for(size_t i = 0; i < RECORDS; ++i) if(!records[i].ptr) {
        records[i].ptr = ptr; records[i].size = size; ++live; return;
    }
    assert(!"Test allocation table exhausted");
}

static bool fail(size_t size)
{
    const bool injected = fail_next;
    fail_next = false;
    return injected || size > 1024U * 1024U;
}

void *heap_caps_malloc(size_t size, uint32_t caps)
{
    assert(caps == UI_CAPS); ++malloc_calls;
    if(!size || fail(size)) return NULL;
    void *ptr = malloc(size); assert(ptr); remember(ptr, size); return ptr;
}

void heap_caps_free(void *ptr)
{
    if(!ptr) return;
    const size_t i = find(ptr);
    assert(live); --live; ++free_calls;
    free(ptr); records[i].ptr = NULL; records[i].size = 0;
}

void *heap_caps_realloc(void *ptr, size_t size, uint32_t caps)
{
    assert(caps == UI_CAPS); ++realloc_calls;
    if(!ptr) return heap_caps_malloc(size, caps);
    if(!size) { heap_caps_free(ptr); return NULL; }
    const size_t i = find(ptr);
    if(fail(size)) return NULL; /* Keep the original block and its bytes. */
    void *next = realloc(ptr, size); assert(next && (uintptr_t)next % 4 == 0);
    records[i].ptr = next; records[i].size = size; return next;
}

bool heap_caps_check_integrity(uint32_t caps, bool print_errors)
{
    assert(caps == UI_CAPS && !print_errors); ++integrity_calls; return integrity_ok;
}

static void small_objects(void)
{
    void *objects[1000];
    const size_t initial_calls = malloc_calls;
    for(size_t i = 0; i < 1000; ++i) {
        const size_t size = 1 + i % 511;
        objects[i] = lv_malloc(size); assert(objects[i]);
        memset(objects[i], (int)(i & 255), size);
    }
    assert(live == 1000 && malloc_calls == initial_calls + 1000);
    for(size_t i = 0; i < 1000; ++i) {
        assert(((uint8_t *)objects[i])[i % (1 + i % 511)] == (i & 255));
        lv_free(objects[i]);
    }
    assert(!live);
    puts("PASS real LVGL public allocation routes 1000 small UI blocks only to SPIRAM|8BIT, keeps four-byte alignment and frees every block");
}

static void reallocations(void)
{
    uint8_t *p = lv_realloc(NULL, 32); assert(p && live == 1);
    for(size_t i = 0; i < 32; ++i) p[i] = (uint8_t)(i * 3);
    p = lv_realloc(p, 512); assert(p);
    for(size_t i = 0; i < 32; ++i) assert(p[i] == i * 3);
    p = lv_realloc(p, 17); assert(p);
    for(size_t i = 0; i < 17; ++i) assert(p[i] == i * 3);
    const size_t old_frees = free_calls;
    fail_next = true; assert(lv_realloc(p, 1024) == NULL && live == 1 && free_calls == old_frees);
    for(size_t i = 0; i < 17; ++i) assert(p[i] == i * 3);
    assert(lv_realloc(p, SIZE_MAX) == NULL && live == 1 && free_calls == old_frees);
    for(size_t i = 0; i < 17; ++i) assert(p[i] == i * 3);
    p = lv_realloc(p, 64); assert(p); lv_free(p); assert(!live);
    p = lv_malloc(24); assert(p); fail_next = true;
    assert(lv_reallocf(p, 512) == NULL && !live && free_calls == old_frees + 2);
    fail_next = true; const size_t attempts = malloc_calls;
    assert(lv_malloc(32) == NULL && malloc_calls == attempts + 1 && !live);
    puts("PASS real public grow/shrink preserve content; realloc OOM/oversize retain the original block, reallocf frees on failure, and OOM never falls back to HP");
}

static void zero_and_helpers(void)
{
    const size_t attempts = malloc_calls, resized = realloc_calls, frees = free_calls;
    void *zero = lv_malloc(0);
    assert(zero && zero == lv_malloc_zeroed(0) && zero == lv_calloc(0, 7));
    assert(zero == lv_realloc(NULL, 0));
    lv_free(zero); lv_free(NULL);
    assert(malloc_calls == attempts && realloc_calls == resized && free_calls == frees);
    uint8_t *p = lv_realloc(zero, 19); assert(p && live == 1);
    memset(p, 123, 19);
    assert(lv_realloc(p, 0) == zero && !live);
    p = lv_malloc_zeroed(49); assert(p);
    for(size_t i = 0; i < 49; ++i) assert(!p[i]);
    lv_free(p);
    p = lv_calloc(7, 11); assert(p);
    for(size_t i = 0; i < 77; ++i) assert(!p[i]);
    lv_free(p);
    p = lv_zalloc(23); assert(p);
    for(size_t i = 0; i < 23; ++i) assert(!p[i]);
    lv_free(p); assert(!live);
    puts("PASS actual LVGL zero sentinel, realloc-zero, free(NULL), calloc/zalloc and malloc-zeroed semantics remain intact");
}

static void unsupported_and_integrity(void)
{
    uint8_t pool[128];
    lv_mem_init(); lv_mem_init();
    void *p = lv_malloc(12); assert(p);
    assert(lv_mem_add_pool(pool, sizeof(pool)) == NULL);
    lv_mem_remove_pool(NULL); lv_mem_remove_pool(pool);
    lv_mem_deinit(); lv_mem_deinit(); assert(live == 1);
    lv_free(p);
    lv_mem_monitor_t monitor, zero;
    memset(&monitor, 0xa5, sizeof(monitor)); memset(&zero, 0, sizeof(zero));
    lv_mem_monitor(&monitor); assert(!memcmp(&monitor, &zero, sizeof(monitor)));
    assert(lv_mem_test() == LV_RESULT_OK && integrity_calls == 1);
    integrity_ok = false;
    assert(lv_mem_test() == LV_RESULT_INVALID && integrity_calls == 2);
    integrity_ok = true;
    /* The real public wrapper detects a modified zero sentinel before backend testing. */
    lv_global.memory_zero = 0;
    assert(lv_mem_test() == LV_RESULT_INVALID && integrity_calls == 2);
    lv_global.memory_zero = ZERO_MEM_SENTINEL;
    assert(!live);
    puts("PASS CLIB-compatible init/deinit/pool/monitor no-ops and explicit whole-PSRAM integrity success/failure; public sentinel corruption is detected");
}

int main(void)
{
    small_objects(); reallocations(); zero_and_helpers(); unsupported_and_integrity();
    assert(!live && malloc_calls > 1000 && realloc_calls && !fail_next);
    puts("All LVGL custom-memory public-wrapper tests passed; host caps are modeled, native PSRAM placement/performance remain board acceptance.");
    return 0;
}
