/* Actual LVGL partial rendering, production stage/sequence wrapper and rounder.
 * Only heap capabilities, SPI DMA completion and RTOS blocking are substituted. */
#include "app_display_metrics.h"
#include "app_display_sync.h"
#include "co5300_display.h"
#include "esp_heap_caps.h"
#include "freertos/semphr.h"
#include "src/display/lv_display_private.h"
#include <assert.h>
#include <errno.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define CAPACITY (480U * 20U * 2U)
struct stage_semaphore { pthread_mutex_t mutex; pthread_cond_t ready; bool token; unsigned waiters; };
static esp_lcd_panel_io_callbacks_t callbacks;
static lv_display_t *registered_display;
static void *mirror;
static pthread_mutex_t job_lock = PTHREAD_MUTEX_INITIALIZER;
static pthread_cond_t job_ready = PTHREAD_COND_INITIALIZER;
static bool pending, quitting;
static uint8_t expected[CAPACITY];
static size_t expected_bytes;
static atomic_bool dma_active, pause_ready, pause_give;
static atomic_uint submissions, completions, final_submissions, final_completions, te_waits;
static unsigned render_cycles, first_te, first_last, tall_regions;
static uint64_t copied_bytes;

static void delay(unsigned ms)
{ struct timespec t={ms/1000,(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
int64_t esp_timer_get_time(void)
{ struct timespec t; clock_gettime(CLOCK_MONOTONIC,&t); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
void test_display_log(const char *tag,const char *format,...){ (void)tag;(void)format; }
const char *esp_err_to_name(esp_err_t error){ (void)error;return "mock"; }
void *heap_caps_aligned_alloc(size_t alignment,size_t bytes,uint32_t caps)
{
    assert(alignment==64 && bytes==CAPACITY && caps==(MALLOC_CAP_INTERNAL|MALLOC_CAP_DMA|MALLOC_CAP_8BIT));
    assert(!mirror); mirror=aligned_alloc(alignment,bytes); assert(mirror); return mirror;
}
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    SemaphoreHandle_t s=calloc(1,sizeof(*s)); assert(s);
    assert(!pthread_mutex_init(&s->mutex,NULL)); assert(!pthread_cond_init(&s->ready,NULL)); return s;
}
void vSemaphoreDelete(SemaphoreHandle_t s)
{ pthread_mutex_destroy(&s->mutex);pthread_cond_destroy(&s->ready);free(s); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ms)
{
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=(long)ms*1000000;
    until.tv_sec+=until.tv_nsec/1000000000;until.tv_nsec%=1000000000;
    pthread_mutex_lock(&s->mutex);++s->waiters;
    int result=0;while(!s->token && result!=ETIMEDOUT) { result=pthread_cond_timedwait(&s->ready,&s->mutex,&until);assert(!result||result==ETIMEDOUT); }
    bool token=s->token;s->token=false;--s->waiters;pthread_mutex_unlock(&s->mutex);return token?pdTRUE:pdFALSE;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *woken)
{
    if(atomic_exchange(&pause_give,false)) delay(8);
    pthread_mutex_lock(&s->mutex);bool added=!s->token;s->token=true;*woken=added&&s->waiters;
    pthread_cond_signal(&s->ready);pthread_mutex_unlock(&s->mutex);return added;
}
void lvgl_port_flush_ready(lv_display_t *display)
{ assert(display==registered_display);lv_display_flush_ready(display);if(atomic_exchange(&pause_ready,false))delay(8); }
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_io_callbacks_t *cb,void *context)
{ assert(io==(void*)1 && cb && context);callbacks=*cb;registered_display=context;return ESP_OK; }
esp_err_t co5300_display_wait_te(esp_lcd_panel_io_handle_t io,uint32_t timeout)
{ assert(io==(void*)1&&timeout==35);++te_waits;return ESP_OK; }
esp_err_t co5300_display_get_te_metrics(esp_lcd_panel_io_handle_t io,co5300_te_metrics_t *metrics)
{ assert(io==(void*)1);*metrics=(co5300_te_metrics_t){.available=true,.edges=te_waits};return ESP_OK; }

static void *dma_worker(void *unused)
{
    (void)unused;
    uint8_t snapshot[CAPACITY];
    for(;;) {
        pthread_mutex_lock(&job_lock);
        while(!pending&&!quitting)pthread_cond_wait(&job_ready,&job_lock);
        if(quitting){pthread_mutex_unlock(&job_lock);return NULL;}
        const size_t bytes=expected_bytes;memcpy(snapshot,expected,bytes);pending=false;
        const bool last=lv_display_flush_is_last(registered_display);
        pthread_mutex_unlock(&job_lock);
        /* DMA owns this one mirror throughout the delayed transfer. */
        for(unsigned i=0;i<3;++i){delay(1);assert(!memcmp(mirror,snapshot,bytes));}
        atomic_store(&dma_active,false);++completions;if(last)++final_completions;
        callbacks.on_color_trans_done((void*)1,NULL,registered_display);
    }
}
static void original_flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
    assert(pixels==mirror && !atomic_exchange(&dma_active,true));
    assert(area->x1>=0&&area->y1>=0&&area->x2<480&&area->y2<480);
    assert(!(area->x1&1)&&!(area->y1&1)&&(area->x2&1)&&(area->y2&1));
    const unsigned width=(unsigned)lv_area_get_width(area),height=(unsigned)lv_area_get_height(area);
    const size_t bytes=(size_t)width*height*2;assert(bytes&&bytes<=CAPACITY);
    lv_draw_buf_t *source=lv_display_get_buf_active(display);
    assert(source&&source->data!=mirror&&source->header.stride==width*2&&source->header.w==width&&source->header.h>=height);
    assert(!memcmp(source->data,pixels,bytes));
    uint8_t source_snapshot[CAPACITY];memcpy(source_snapshot,source->data,bytes);
    /* Same single in-place swap as the installed esp_lvgl_port flush. */
    lv_draw_sw_rgb565_swap(pixels,width*height);
    assert(!memcmp(source->data,source_snapshot,bytes));
    for(size_t i=0;i<bytes;i+=2){assert(pixels[i]==source_snapshot[i+1]);assert(pixels[i+1]==source_snapshot[i]);}
    ++submissions;if(lv_display_flush_is_last(display))++final_submissions;
    if(height>20&&width<480)++tall_regions;
    copied_bytes+=bytes;
    pthread_mutex_lock(&job_lock);assert(!pending);memcpy(expected,pixels,bytes);expected_bytes=bytes;pending=true;
    pthread_cond_signal(&job_ready);pthread_mutex_unlock(&job_lock);
}
static void round_event(lv_event_t *event){app_display_rounder(lv_event_get_param(event));}
static void observe(lv_event_t *event)
{
    if(lv_event_get_code(event)==LV_EVENT_RENDER_START){++render_cycles;first_te=te_waits;first_last=final_submissions;}
    if(lv_event_get_code(event)==LV_EVENT_RENDER_READY){assert(te_waits==first_te+1);assert(final_submissions==first_last+1);}
}
static void refresh(lv_obj_t *object)
{lv_obj_invalidate(object);lv_tick_inc(10);lv_refr_now(NULL);}
static void drain(lv_display_t *display)
{
    display->flush_wait_cb(display);
    app_display_metrics_t m;app_display_metrics_get(&m);
    assert(m.submitted_sequence==m.completed_sequence&&!m.flush_waiting&&!m.dropped_regions);
    assert(m.staging_available&&m.staging_capacity==CAPACITY&&m.staging_copied_bytes==copied_bytes);
    assert(m.completed_regions==submissions&&m.completed_refreshes==render_cycles);
    assert(!dma_active&&completions==submissions&&final_completions==render_cycles);
}
int main(void)
{
    lv_init();lv_display_t *display=lv_display_create(480,480);assert(display);
    uint8_t *source1=aligned_alloc(64,CAPACITY),*source2=aligned_alloc(64,CAPACITY);assert(source1&&source2);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,source1,source2,CAPACITY,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,original_flush);
    lv_display_add_event_cb(display,round_event,LV_EVENT_INVALIDATE_AREA,NULL);
    assert(app_display_metrics_attach_staged_rgb565((void*)1,display,CAPACITY)==ESP_OK);
    assert(app_display_sync_attach(display,(void*)1)==ESP_OK);
    lv_display_add_event_cb(display,observe,LV_EVENT_ALL,NULL);app_display_metrics_set_active(true);
    pthread_t worker;assert(!pthread_create(&worker,NULL,dma_worker,NULL));
    refresh(lv_screen_active());drain(display);assert(submissions==24&&final_completions==1);
    puts("PASS actual LVGL partial20 double buffer: 24 real flushes, sole final completion, stable DMA mirror and unchanged source bytes");
    lv_obj_t *object=lv_obj_create(lv_screen_active());lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object,lv_color_hex(0x3174A8),0);lv_obj_set_style_bg_opa(object,LV_OPA_COVER,0);
    lv_obj_set_pos(object,1,1);lv_obj_set_size(object,17,475);refresh(object);drain(display);
    refresh(object);drain(display);assert(tall_regions);
    puts("PASS real narrow/tall 2-by-2 rounded regions exceed 20 rows with packed stride and remain within 19200 bytes");
    for(unsigned i=0;i<120;++i) {
        if(i==8)atomic_store(&pause_ready,true);
        if(i==16)atomic_store(&pause_give,true);
        lv_obj_set_pos(object,(int32_t)(i*71%460),(int32_t)(i*37%460));
        lv_obj_set_size(object,(int32_t)(i*43%259+1),(int32_t)(i*19%479+1));refresh(object);
    }
    drain(display);assert(!pause_ready&&!pause_give);
    puts("PASS odd/clipped/window changes plus delayed old ready/publication and stale give never overwrite in-flight mirror or miscount refreshes");
    pthread_mutex_lock(&job_lock);quitting=true;pthread_cond_signal(&job_ready);pthread_mutex_unlock(&job_lock);
    assert(!pthread_join(worker,NULL));assert(lv_mem_test()==LV_RESULT_OK);
    lv_display_delete(display);free(source1);free(source2);lv_deinit();
    puts("All production stage-copy + real LVGL rendering regressions passed.");return 0;
}
