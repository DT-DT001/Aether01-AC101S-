/* LVGL's supported custom allocator keeps UI objects out of the HP/DMA heap.
 * Display DMA buffers and task stacks retain their separate port allocation. */
#include "lvgl.h"
#include "esp_heap_caps.h"

#if LV_USE_STDLIB_MALLOC != LV_STDLIB_CUSTOM
#error "The project LVGL allocator requires LV_STDLIB_CUSTOM"
#endif

#define UI_MEMORY_CAPS (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)

void lv_mem_init(void)
{
    /* IDF owns heap initialization. */
}

void lv_mem_deinit(void)
{
    /* Match LVGL's CLIB backend: never reset the shared IDF heap. */
}

lv_mem_pool_t lv_mem_add_pool(void *mem, size_t bytes)
{
    (void)mem;
    (void)bytes;
    return NULL;
}

void lv_mem_remove_pool(lv_mem_pool_t pool)
{
    (void)pool;
}

void *lv_malloc_core(size_t size)
{
    return heap_caps_malloc(size, UI_MEMORY_CAPS);
}

void lv_free_core(void *p)
{
    heap_caps_free(p);
}

void *lv_realloc_core(void *p, size_t new_size)
{
    /* IDF preserves the old allocation/content when reallocation fails. */
    return heap_caps_realloc(p, new_size, UI_MEMORY_CAPS);
}

void lv_mem_monitor_core(lv_mem_monitor_t *mon_p)
{
    /* Unsupported, as in CLIB. lv_mem_monitor() already zeroes the snapshot.
     * Shared PSRAM heap totals would not describe LVGL's own usage. */
    (void)mon_p;
}

lv_result_t lv_mem_test_core(void)
{
    /* Explicit diagnostics only: this checks all PSRAM, not just UI objects.
     * No integrity walk is performed by malloc/free/realloc. */
    return heap_caps_check_integrity(UI_MEMORY_CAPS, false) ? LV_RESULT_OK : LV_RESULT_INVALID;
}
