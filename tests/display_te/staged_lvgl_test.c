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
static bool pending, quitting, pending_last, pending_direct;
static uint8_t expected[CAPACITY];
static size_t expected_bytes;
static const uint8_t *pending_pixels;
static const uint16_t *direct_source;
static unsigned direct_refreshes,expected_drops;
static unsigned raw_params,raw_colors,raw_fail_color,raw_fail_command;
static int raw_x1,raw_x2,raw_y1,raw_y2;
static atomic_bool dma_active, pause_ready, pause_give;
static atomic_uint submissions, completions, final_submissions, final_completions, te_waits;
static int64_t last_te_edge_us;
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
{ assert(io==(void*)1&&timeout==35);++te_waits;last_te_edge_us=esp_timer_get_time();return ESP_OK; }
esp_err_t co5300_display_get_te_metrics(esp_lcd_panel_io_handle_t io,co5300_te_metrics_t *metrics)
{
    assert(io==(void*)1);
    *metrics=(co5300_te_metrics_t){.available=true,.edges=te_waits,
        .last_period_us=16667,.last_edge_us=last_te_edge_us};
    return ESP_OK;
}

static void *dma_worker(void *unused)
{
    (void)unused;
    uint8_t snapshot[CAPACITY];
    for(;;) {
        pthread_mutex_lock(&job_lock);
        while(!pending&&!quitting)pthread_cond_wait(&job_ready,&job_lock);
        if(quitting){pthread_mutex_unlock(&job_lock);return NULL;}
        const size_t bytes=expected_bytes;memcpy(snapshot,expected,bytes);pending=false;
        const bool last=pending_last, direct=pending_direct;
        const uint8_t *active_pixels=pending_pixels;
        pthread_mutex_unlock(&job_lock);
        /* DMA owns this one mirror throughout the delayed transfer. */
        if (direct) assert(!memcmp(active_pixels,snapshot,bytes));
        else for(unsigned i=0;i<3;++i){delay(1);assert(!memcmp(active_pixels,snapshot,bytes));}
        atomic_store(&dma_active,false);++completions;if(last)++final_completions;
        callbacks.on_color_trans_done((void*)1,NULL,registered_display);
    }
}
static void original_flush(lv_display_t *display,const lv_area_t *area,uint8_t *pixels)
{
    (void)display;(void)area;(void)pixels;
    abort();
}
esp_err_t esp_lcd_panel_draw_bitmap(esp_lcd_panel_handle_t panel,int x1,int y1,int x2,int y2,const void *data)
{
    assert(panel==(void*)2);
    lv_display_t *display=registered_display;
    lv_area_t coordinates={x1,y1,x2-1,y2-1};
    const lv_area_t *area=&coordinates;
    const uint8_t *pixels=data;
    if (direct_source) {
        assert(pixels==(const uint8_t *)mirror+((raw_colors-1U)&1U)*(CAPACITY/2U));
    } else assert(pixels==mirror);
    assert(!atomic_exchange(&dma_active,true));
    assert(area->x1>=0&&area->y1>=0&&area->x2<480&&area->y2<480);
    assert(!(area->x1&1)&&!(area->y1&1)&&(area->x2&1)&&(area->y2&1));
    const unsigned width=(unsigned)lv_area_get_width(area),height=(unsigned)lv_area_get_height(area);
    const size_t bytes=(size_t)width*height*2;assert(bytes&&bytes<=CAPACITY);
    const uint8_t *source_pixels;
    bool last;
    if (direct_source) {
        assert(x1==48&&x2==432&&y1>=96&&y2<=432);
        source_pixels=(const uint8_t *)(direct_source+(y1-96)*384);
        last=y2==432;
    } else {
        lv_draw_buf_t *source=lv_display_get_buf_active(display);
        assert(source&&source->data!=mirror&&source->header.stride==width*2&&source->header.w==width&&source->header.h>=height);
        source_pixels=source->data;
        last=lv_display_flush_is_last(display);
    }
    for(size_t i=0;i<bytes;i+=2){assert(pixels[i]==source_pixels[i+1]);assert(pixels[i+1]==source_pixels[i]);}
    ++submissions;if(last)++final_submissions;
    if(height>20&&width<480)++tall_regions;
    copied_bytes+=bytes;
    pthread_mutex_lock(&job_lock);assert(!pending);memcpy(expected,pixels,bytes);expected_bytes=bytes;pending=true;
    pending_last=last;pending_direct=direct_source!=NULL;pending_pixels=pixels;
    pthread_cond_signal(&job_ready);pthread_mutex_unlock(&job_lock);
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_param(esp_lcd_panel_io_handle_t io,int command,const void *data,size_t bytes)
{
    assert(io==(void*)1&&data&&bytes==4);
    ++raw_params;
    if ((unsigned)command==raw_fail_command) return ESP_FAIL;
    const uint8_t *param=data;
    if (command==0x02002A00) {
        raw_x1=(param[0]<<8)|param[1];
        raw_x2=((param[2]<<8)|param[3])+1;
    } else {
        assert(command==0x02002B00);
        raw_y1=(param[0]<<8)|param[1];
        raw_y2=((param[2]<<8)|param[3])+1;
    }
    return ESP_OK;
}

esp_err_t esp_lcd_panel_io_tx_color(esp_lcd_panel_io_handle_t io,int command,const void *data,size_t bytes)
{
    assert(io==(void*)1&&direct_source&&raw_params==2&&raw_x1==48&&raw_x2==432);
    assert(command==(raw_colors?0x32003C00:0x32002C00));
    ++raw_colors;
    if (raw_colors==raw_fail_color) {
        copied_bytes+=bytes;
        ++expected_drops;
        return ESP_FAIL;
    }
    const int y_end=raw_y1+(int)(bytes/768U);
    assert(bytes==9216&&y_end<=raw_y2);
    esp_err_t error=esp_lcd_panel_draw_bitmap((void*)2,raw_x1,raw_y1,raw_x2,y_end,data);
    raw_y1=y_end;
    return error;
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
    assert(m.submitted_sequence==m.completed_sequence&&!m.flush_waiting&&m.dropped_regions==expected_drops);
    assert(m.staging_available&&m.staging_capacity==CAPACITY&&m.staging_copied_bytes==copied_bytes);
    assert(m.completed_regions==submissions&&m.completed_refreshes==render_cycles+direct_refreshes);
    assert(!dma_active&&completions==submissions&&final_completions==render_cycles+direct_refreshes);
}
int main(void)
{
    lv_init();lv_display_t *display=lv_display_create(480,480);assert(display);
    uint8_t *source1=aligned_alloc(64,CAPACITY),*source2=aligned_alloc(64,CAPACITY);assert(source1&&source2);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,source1,source2,CAPACITY,LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,original_flush);
    lv_display_add_event_cb(display,round_event,LV_EVENT_INVALIDATE_AREA,NULL);
    assert(app_display_metrics_attach_staged_rgb565((void*)1,(void*)2,display,CAPACITY)==ESP_OK);
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

    const lv_area_t nes_area={48,96,431,431};
    lv_obj_set_pos(object,nes_area.x1,nes_area.y1);
    lv_obj_set_size(object,384,336);
    refresh(lv_screen_active());drain(display);
    app_display_sync_set_nes_viewport(display,&nes_area);
    const unsigned regions_before=submissions;
    refresh(object);drain(display);
    app_display_metrics_t timing;
    app_display_metrics_get(&timing);
    assert(submissions==regions_before+14);
    /* A mocked 3 ms per strip intentionally crosses the panel scan estimate. */
    assert(timing.nes.timed_frames==1&&timing.nes.late_frames==1&&
        timing.nes.max_te_to_done_us>=42000&&timing.nes.min_scan_margin_us<0);
    app_display_sync_set_nes_viewport(display,NULL);
    refresh(object);drain(display);
    app_display_metrics_get(&timing);assert(timing.nes.timed_frames==1);
    puts("PASS NES viewport waits on actual TE anchor and measures all 14 staged strips through the final asynchronous completion");

    lv_obj_t *screen=lv_screen_active();
    lv_obj_t *container=lv_obj_create(screen);
    lv_obj_remove_style_all(container);
    lv_obj_set_size(container,480,480);
    lv_obj_set_parent(object,container);
    lv_obj_set_pos(object,48,96);
    refresh(screen);drain(display);
    app_display_sync_set_nes_viewport(display,&nes_area);
    uint16_t *raw=malloc(384U*336U*sizeof(*raw));assert(raw);
    for(size_t i=0;i<384U*336U;++i)raw[i]=(uint16_t)(i*37U);
    co5300_window_t bad={48,96,431,432};
    assert(co5300_display_write_rgb565_strip((void*)1,&bad,raw,9216)==ESP_ERR_INVALID_ARG);
    assert(!raw_params&&!raw_colors);
    const co5300_window_t window={48,96,432,432};
    raw_fail_command=0x02002A00;
    assert(co5300_display_write_rgb565_strip((void*)1,&window,raw,9216)==ESP_FAIL);
    assert(raw_params==1&&!raw_colors);
    raw_params=0;
    raw_fail_command=0x02002B00;
    assert(co5300_display_write_rgb565_strip((void*)1,&window,raw,9216)==ESP_FAIL);
    assert(raw_params==2&&!raw_colors);
    raw_fail_command=raw_params=0;
    direct_source=raw;
    const unsigned direct_first=submissions,direct_te=te_waits,loops_before=render_cycles;
    assert(app_display_sync_present_nes(object,raw,384,336));
    ++direct_refreshes;
    direct_source=NULL;
    drain(display);
    app_display_metrics_get(&timing);
    assert(submissions==direct_first+28&&te_waits==direct_te+1&&render_cycles==loops_before);
    assert(raw_params==2&&raw_colors==28&&raw_y1==432);
    assert(timing.nes.direct_frames==1&&timing.nes.direct_timed_frames==1&&timing.nes.timed_frames==1);
    raw_params=raw_colors=0;
    raw_fail_color=5;
    direct_source=raw;
    assert(!app_display_sync_present_nes(object,raw,384,336));
    direct_source=NULL;
    drain(display);
    app_display_metrics_get(&timing);
    assert(raw_params==2&&raw_colors==5&&timing.nes.direct_frames==1&&timing.nes.direct_timed_frames==1);
    lv_obj_t *overlay=lv_obj_create(screen);
    lv_obj_set_size(overlay,50,25);
    assert(!app_display_sync_present_nes(object,raw,384,336)); /* Pending invalidation. */
    refresh(screen);drain(display);
    assert(!app_display_sync_present_nes(object,raw,384,336)); /* Visible after refresh. */
    lv_obj_delete(overlay);
    refresh(screen);drain(display);
    assert(!app_display_sync_present_nes(object,raw,382,336)); /* Unsupported source. */
    lv_obj_t *top_overlay=lv_obj_create(lv_layer_top());
    refresh(screen);drain(display);
    assert(!app_display_sync_present_nes(object,raw,384,336));
    lv_obj_delete(top_overlay);
    app_display_sync_set_nes_viewport(display,NULL);
    free(raw);
    puts("PASS exact fullscreen NES uses one address window, one TE, 28 immutable alternating strips, and recovers cleanly after parameter/color errors and overlays");

    pthread_mutex_lock(&job_lock);quitting=true;pthread_cond_signal(&job_ready);pthread_mutex_unlock(&job_lock);
    assert(!pthread_join(worker,NULL));assert(lv_mem_test()==LV_RESULT_OK);
    lv_display_delete(display);free(source1);free(source2);lv_deinit();
    puts("All production stage-copy + real LVGL rendering regressions passed.");return 0;
}
