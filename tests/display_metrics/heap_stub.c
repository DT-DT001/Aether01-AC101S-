#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include <assert.h>
#include <stdlib.h>
#include <stdint.h>
bool test_heap_fail;
unsigned test_heap_live;
static void *allocations[16];
esp_err_t (*test_panel_draw)(esp_lcd_panel_handle_t, int, int, int, int, const void *);
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel, int x1, int y1, int x2, int y2, const void *pixels)
{
    assert(test_panel_draw);
    return test_panel_draw(panel,x1,y1,x2,y2,pixels);
}
void *heap_caps_aligned_alloc(size_t alignment,size_t bytes,uint32_t caps)
{
    assert(alignment==64&&!(bytes%64)&&caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
    if(test_heap_fail)return NULL;
#ifdef _WIN32
    void *p=_aligned_malloc(bytes,alignment);
#else
    void *p=aligned_alloc(alignment,bytes);
#endif
    assert(p&&!((uintptr_t)p%64));
    for(unsigned i=0;i<16;++i)if(!allocations[i]) { allocations[i]=p;++test_heap_live;return p; }
    abort();
}
void __real_free(void *);
void __wrap_free(void *p)
{
    if(p)for(unsigned i=0;i<16;++i)if(allocations[i]==p) {
        allocations[i]=NULL;--test_heap_live;
#ifdef _WIN32
        _aligned_free(p);
        return;
#else
        break;
#endif
    }
    __real_free(p);
}
