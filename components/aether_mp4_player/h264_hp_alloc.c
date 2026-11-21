/* Project-side allocation policy for the pinned ESP-H264 1.4.1 S31 archive.
 * Only its three renamed undefined references reach these functions. Do not
 * wrap the system's malloc/heap_caps_* symbols globally. The S31 PIE cannot
 * access LP SRAM, although that region advertises MALLOC_CAP_INTERNAL. */
#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_h264_alloc.h"

void *aether_h264_hp_malloc_prefer(size_t size, size_t count, ...);
void *aether_h264_hp_calloc_prefer(size_t n, size_t size, size_t count, ...);
void *aether_h264_hp_malloc(size_t size);
void *__wrap_esp_h264_calloc_prefer(uint32_t n, uint32_t size,
    uint32_t *actual_size, uint32_t caps1, uint32_t caps2);
void *__real_esp_h264_calloc_prefer(uint32_t n, uint32_t size,
    uint32_t *actual_size, uint32_t caps1, uint32_t caps2);

static uint32_t without_lp(uint32_t caps)
{
    return (caps & MALLOC_CAP_INTERNAL) ? caps | MALLOC_CAP_DMA : caps;
}

void *aether_h264_hp_malloc_prefer(size_t size, size_t count, ...)
{
    /* Every call site in the hash-pinned archive passes exactly two caps.
     * Reject a changed ABI without consuming arguments or using a looser heap. */
    if (count != 2) return NULL;
    va_list args;
    va_start(args, count);
    const uint32_t caps1 = va_arg(args, uint32_t);
    const uint32_t caps2 = va_arg(args, uint32_t);
    va_end(args);
    /* One real preference call preserves ordering, zero-size handling and
     * the SDK's single allocation-failure callback after both choices fail. */
    return heap_caps_malloc_prefer(size, 2, without_lp(caps1), without_lp(caps2));
}

void *aether_h264_hp_calloc_prefer(size_t n, size_t size, size_t count, ...)
{
    if (count != 2) return NULL;
    va_list args;
    va_start(args, count);
    const uint32_t caps1 = va_arg(args, uint32_t);
    const uint32_t caps2 = va_arg(args, uint32_t);
    va_end(args);
    return heap_caps_calloc_prefer(n, size, 2, without_lp(caps1), without_lp(caps2));
}

void *aether_h264_hp_malloc(size_t size)
{
    /* h264bsdInit's only plain malloc is 1280 bytes. Keep its current default
     * allocator's >1024-byte PSRAM-first policy, with a safe HP fallback. */
    return heap_caps_malloc_prefer(size, 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT,
        MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
}

void *__wrap_esp_h264_calloc_prefer(uint32_t n, uint32_t size,
    uint32_t *actual_size, uint32_t caps1, uint32_t caps2)
{
    /* This fixed-signature SDK front-end allocates the software-decoder
     * wrapper. Keep its alignment/cache operations and size reporting intact. */
    return __real_esp_h264_calloc_prefer(n, size, actual_size,
        without_lp(caps1), without_lp(caps2));
}
