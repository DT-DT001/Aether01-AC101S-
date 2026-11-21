#include "esp_heap_caps.h"
#include <assert.h>
#include <stdbool.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

void *aether_h264_hp_malloc_prefer(size_t size, size_t count, ...);
void *aether_h264_hp_calloc_prefer(size_t n, size_t size, size_t count, ...);
void *aether_h264_hp_malloc(size_t size);
void *__wrap_esp_h264_calloc_prefer(uint32_t n, uint32_t size,
    uint32_t *actual_size, uint32_t caps1, uint32_t caps2);
void *__real_esp_h264_calloc_prefer(uint32_t n, uint32_t size,
    uint32_t *actual_size, uint32_t caps1, uint32_t caps2);

enum { HP = MALLOC_CAP_INTERNAL | MALLOC_CAP_DMA | MALLOC_CAP_8BIT,
       LP_OK = MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT,
       PSRAM = MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT };
static unsigned char hp[8], lp[8], psram[8];
static bool hp_fail, psram_fail;
static struct { unsigned calls, attempts, oom; bool zeroed;
    size_t n, size; uint32_t caps[2]; uint32_t *actual; } seen;
static void reset(void) { memset(&seen, 0, sizeof(seen)); hp_fail=psram_fail=false; }
static void *allocate(size_t n, size_t size, bool zeroed, uint32_t first, uint32_t second)
{
    ++seen.calls; seen.n=n; seen.size=size; seen.zeroed=zeroed;
    seen.caps[0]=first; seen.caps[1]=second;
    for (unsigned i=0; i<2; ++i) {
        ++seen.attempts;
        if (seen.caps[i] & MALLOC_CAP_SPIRAM) { if (!psram_fail) return psram; }
        else if (seen.caps[i] & MALLOC_CAP_INTERNAL) {
            if (!(seen.caps[i] & MALLOC_CAP_DMA)) return lp;
            if (!hp_fail) return hp;
        }
    }
    ++seen.oom; return NULL; /* The real SDK calls its hook only here. */
}
void *heap_caps_malloc_prefer(size_t size, size_t count, ...)
{
    assert(count==2); va_list args; va_start(args,count);
    uint32_t first=va_arg(args,uint32_t),second=va_arg(args,uint32_t); va_end(args);
    return allocate(1,size,false,first,second);
}
void *heap_caps_calloc_prefer(size_t n, size_t size, size_t count, ...)
{
    assert(count==2); va_list args; va_start(args,count);
    uint32_t first=va_arg(args,uint32_t),second=va_arg(args,uint32_t); va_end(args);
    return allocate(n,size,true,first,second);
}
void *__real_esp_h264_calloc_prefer(uint32_t n,uint32_t size,uint32_t *actual,
                                  uint32_t first,uint32_t second)
{
    seen.actual=actual; *actual=320;
    return allocate(n,size,true,first,second);
}
int main(void)
{
    reset(); assert(aether_h264_hp_malloc_prefer(5120,2,LP_OK,PSRAM)==hp);
    assert(seen.calls==1 && seen.attempts==1 && seen.caps[0]==HP && seen.caps[1]==PSRAM);
    reset(); hp_fail=true;
    assert(aether_h264_hp_malloc_prefer(5120,2,LP_OK,PSRAM)==psram);
    assert(seen.calls==1 && seen.attempts==2 && !seen.oom);
    reset(); hp_fail=psram_fail=true;
    assert(!aether_h264_hp_malloc_prefer(5120,2,LP_OK,PSRAM));
    assert(seen.calls==1 && seen.attempts==2 && seen.oom==1);
    puts("PASS production malloc-prefer excludes LP, preserves HP/PSRAM order and one final OOM notification");
    reset(); assert(aether_h264_hp_calloc_prefer(7,431,2,LP_OK|MALLOC_CAP_CACHE_ALIGNED,PSRAM)==hp);
    assert(seen.n==7 && seen.size==431 && seen.zeroed && seen.caps[0]==(HP|MALLOC_CAP_CACHE_ALIGNED));
    reset(); hp_fail=true;
    assert(aether_h264_hp_calloc_prefer(7,431,2,LP_OK,PSRAM)==psram && !seen.oom);
    reset(); hp_fail=psram_fail=true;
    assert(!aether_h264_hp_calloc_prefer(7,431,2,LP_OK,PSRAM) && seen.oom==1);
    reset(); assert(aether_h264_hp_malloc_prefer(12,2,PSRAM,LP_OK)==psram);
    assert(seen.caps[0]==PSRAM && seen.caps[1]==HP);
    puts("PASS production calloc preserves count/size/other caps and both preference orders");
    reset(); assert(!aether_h264_hp_malloc_prefer(12,0));
    assert(!aether_h264_hp_malloc_prefer(12,1));
    assert(!aether_h264_hp_malloc_prefer(12,3));
    assert(!aether_h264_hp_calloc_prefer(7,12,0));
    assert(!aether_h264_hp_calloc_prefer(7,12,1));
    assert(!aether_h264_hp_calloc_prefer(7,12,3));
    assert(!seen.calls);
    puts("PASS changed preference count fails closed before reading varargs or touching any heap");
    reset(); assert(aether_h264_hp_malloc(1280)==psram);
    assert(seen.size==1280 && seen.caps[0]==PSRAM && seen.caps[1]==HP);
    reset(); psram_fail=true; assert(aether_h264_hp_malloc(1280)==hp && !seen.oom);
    reset(); hp_fail=psram_fail=true; assert(!aether_h264_hp_malloc(1280) && seen.oom==1);
    puts("PASS the decoder's plain malloc is PSRAM-first with safe HP fallback");
    uint32_t actual=0; reset();
    assert(__wrap_esp_h264_calloc_prefer(1,36,&actual,PSRAM,LP_OK)==psram);
    assert(actual==320 && seen.actual==&actual && seen.n==1 && seen.size==36 && seen.zeroed);
    assert(seen.caps[0]==PSRAM && seen.caps[1]==HP);
    reset(); assert(heap_caps_malloc_prefer(32,2,LP_OK,PSRAM)==lp);
    puts("PASS front-end wrapper preserves size-report pointer and ordinary non-H264 allocations remain unchanged");
    return 0;
}
