#include "esp_heap_caps.h"
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
bool test_heap_fail;
unsigned test_heap_live;
static void *allocations[16];
void *heap_caps_aligned_alloc(size_t alignment,size_t bytes,uint32_t caps)
{
    assert(alignment==64&&!(bytes%64)&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
    if(test_heap_fail)return NULL;
    void *p=aligned_alloc(alignment,bytes);assert(p&&!((uintptr_t)p%64));
    for(unsigned i=0;i<16;++i)if(!allocations[i]) { allocations[i]=p;++test_heap_live;return p; }
    abort();
}
void __real_free(void *);
void __wrap_free(void *p)
{
    if(p)for(unsigned i=0;i<16;++i)if(allocations[i]==p) { allocations[i]=NULL;--test_heap_live;break; }
    __real_free(p);
}
