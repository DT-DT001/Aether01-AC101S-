/* Reuse the pthread/ISR scheduling endpoint harness; execute a separate
 * staged suite against a fresh production singleton in this process. */
#define main nonstaged_endpoint_example
#include "wait_test.c"
#undef main
#include "esp_heap_caps.h"
#include "esp_lcd_panel_ops.h"
#include <string.h>

static _Alignas(uint16_t) uint8_t source_a[32000],source_b[32000];
static uint8_t source_snapshot[32000],expected[19200];
static const uint8_t *dma_pixels;
static size_t dma_bytes;
static atomic_bool dma_live,submit_done;
static atomic_uint stage_calls;
static lv_draw_buf_t active;
static lv_area_t next_area;
static lv_display_t *staged_display;
static bool fail_draw;
static void stage_flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{
    (void)d;(void)a;(void)p;
    abort();
}
static esp_err_t stage_draw(esp_lcd_panel_handle_t panel,int x1,int y1,int x2,int y2,const void *p)
{
    assert(panel==staged_display&&!atomic_load(&dma_live));
    assert(!((uintptr_t)p%64)&&p!=source_a&&p!=source_b);
    if (fail_draw) return ESP_FAIL;
    dma_bytes=(size_t)(x2-x1)*(size_t)(y2-y1)*2;
    assert(dma_bytes<=sizeof(expected));
    dma_pixels=p;memcpy(expected,p,dma_bytes);atomic_store(&dma_live,true);++stage_calls;
    return ESP_OK;
}
static void prepare(lv_display_t *d,uint8_t *source,const lv_area_t *a)
{
    unsigned width=(unsigned)(a->x2-a->x1+1),height=(unsigned)(a->y2-a->y1+1);
    for(unsigned i=0;i<sizeof(source_a);++i)source[i]=(uint8_t)(i*37+atomic_load(&stage_calls)*13);
    active=(lv_draw_buf_t){.header={.w=width,.h=height,.stride=width*2,.cf=LV_COLOR_FORMAT_RGB565},.data=source,.data_size=sizeof(source_a)};
    d->active=&active;
}
static void staged_submit(lv_display_t *d,const lv_area_t *a,uint8_t *source)
{ atomic_store(&d->flushing,true);d->flush_cb(d,a,source); }
static void staged_complete(void)
{
    assert(atomic_load(&dma_live)&&!memcmp(dma_pixels,expected,dma_bytes));
    atomic_store(&dma_live,false);complete();
}
static void *staged_isr(void *unused) { (void)unused;staged_complete();return NULL; }
static void *staged_submit_thread(void *unused)
{ (void)unused;staged_submit(staged_display,&next_area,source_b);atomic_store(&submit_done,true);return NULL; }
static void assert_invalid(lv_display_t *d,const lv_area_t *a,uint8_t *source)
{
    app_display_metrics_t before=snapshot();unsigned calls=atomic_load(&stage_calls),ready=atomic_load(&ready_calls);
    staged_submit(d,a,source);app_display_metrics_t after=snapshot();
    assert(atomic_load(&stage_calls)==calls&&atomic_load(&ready_calls)==ready+1&&!atomic_load(&d->flushing));
    assert(after.submitted_sequence==before.submitted_sequence&&after.completed_sequence==before.completed_sequence);
    assert(after.completed_regions==before.completed_regions&&after.completed_refreshes==before.completed_refreshes);
    assert(after.dropped_regions==before.dropped_regions+1&&after.staging_error==ESP_ERR_INVALID_ARG);
}

static void nes_timing_frame(lv_display_t *d, int64_t edge, int64_t first_done, int64_t last_done,
                             bool interrupted)
{
    const lv_area_t viewport = {48, 96, 431, 143};
    const lv_area_t top = {48, 96, 431, 119};
    const lv_area_t bottom = {48, interrupted ? 122 : 120, 431, 143};
    app_display_metrics_nes_arm(&viewport, edge, 16667, 480);
    atomic_store(&test_clock_us, edge + 200);
    d->last = false;
    prepare(d, source_a, &top);
    staged_submit(d, &top, source_a);
    atomic_store(&test_clock_us, edge + first_done);
    staged_complete();
    d->last = true;
    prepare(d, source_a, &bottom);
    staged_submit(d, &bottom, source_a);
    atomic_store(&test_clock_us, edge + last_done);
    staged_complete();
}

static void test_nes_timing(lv_display_t *d)
{
    app_display_metrics_nes_reset();
    nes_timing_frame(d, 1000000, 1000, 2000, false);
    app_display_nes_metrics_t m = snapshot().nes;
    assert(m.timed_frames == 1 && m.late_frames == 0);
    assert(m.max_te_to_submit_us == 200 && m.max_te_to_done_us == 2000);
    assert(m.min_scan_margin_us == 143 * 16667 / 480 - 2000);
    nes_timing_frame(d, 2000000, 4300, 4400, false);
    m = snapshot().nes;
    assert(m.timed_frames == 2 && m.late_frames == 1);
    assert(m.max_te_to_done_us == 4400 && m.min_scan_margin_us == 119 * 16667 / 480 - 4300);
    nes_timing_frame(d, 3000000, 7000, 8000, true);
    assert(snapshot().nes.timed_frames == 2); /* A disjoint redraw is not an NES frame. */
    app_display_metrics_nes_reset();
    assert(!snapshot().nes.timed_frames);
    atomic_store(&test_clock_us, 0);
    puts("PASS NES timing uses exact contiguous strips and real DMA completion; a late intermediate strip is detected");
}

static const uint8_t *direct_base;
static esp_err_t direct_te(void *ctx)
{
    assert(direct_base && direct_base[0] == source_a[1] && direct_base[1] == source_a[0]);
    return *(const esp_err_t *)ctx;
}

static esp_err_t direct_strip(void *ctx, const lv_area_t *first_window, const void *pixels, size_t bytes)
{
    (void)ctx;
    static lv_area_t window;
    static int32_t y;
    if (first_window) {
        window = *first_window;
        y = window.y1;
    }
    const int32_t rows = (int32_t)(bytes / (2U * (window.x2 - window.x1 + 1)));
    const esp_err_t error = stage_draw(staged_display, window.x1, y, window.x2+1, y+rows, pixels);
    y += rows;
    return error;
}

static unsigned next_half_verified;
static void verify_while_dma_active(void)
{
    if (!dma_live) return;
    assert(dma_bytes == 9216);
    if (!next_half_verified) {
        assert(dma_pixels == direct_base);
        const uint8_t *next = direct_base + 9600;
        for (size_t i=0; i<dma_bytes; i+=2) {
            assert(next[i]==source_a[dma_bytes+i+1]&&next[i+1]==source_a[dma_bytes+i]);
        }
        assert(!memcmp(dma_pixels,expected,dma_bytes));
    } else {
        assert(dma_pixels == direct_base + 9600);
    }
    ++next_half_verified;
    staged_complete();
}

static void test_direct_nes(lv_display_t *d)
{
    const lv_area_t strip = {48,96,431,119};
    prepare(d,source_a,&strip);
    esp_err_t te_error = ESP_ERR_TIMEOUT;
    const unsigned ready_before = ready_calls;
    app_display_metrics_t before = snapshot();
    direct_base = dma_pixels;
    assert(app_display_metrics_nes_write(d,&strip,(const uint16_t *)source_a,direct_te,direct_strip,&te_error)==ESP_ERR_TIMEOUT);
    assert(snapshot().submitted_sequence==before.submitted_sequence&&!dma_live);
    te_error = ESP_OK;
    before_semaphore_take = verify_while_dma_active;
    assert(app_display_metrics_nes_write(d,&strip,(const uint16_t *)source_a,direct_te,direct_strip,&te_error)==ESP_OK);
    before_semaphore_take = NULL;
    assert(next_half_verified==2);
    app_display_metrics_t after = snapshot();
    assert(after.nes.direct_frames==1&&after.completed_refreshes==before.completed_refreshes+1&&ready_calls==ready_before);

    fail_draw = true;
    assert(app_display_metrics_nes_write(d,&strip,(const uint16_t *)source_a,direct_te,direct_strip,&te_error)==ESP_FAIL);
    fail_draw = false;
    assert(snapshot().submitted_sequence==after.submitted_sequence&&!dma_live);

    assert(app_display_metrics_nes_write(d,&strip,(const uint16_t *)source_a,direct_te,direct_strip,&te_error)==ESP_ERR_TIMEOUT);
    const app_display_metrics_t blocked = snapshot();
    assert(dma_live&&blocked.submitted_sequence==blocked.completed_sequence+1&&blocked.flush_stalled);
    staged_submit(d,&strip,source_a);
    assert(!d->flushing&&dma_live&&snapshot().submitted_sequence==blocked.submitted_sequence);
    assert(ready_calls==ready_before+1);
    d->flushing = true;
    staged_complete();
    assert(d->flushing&&ready_calls==ready_before+1); /* Readiness belongs to the current LVGL draw. */
    d->flushing = false;
    staged_submit(d,&strip,source_a);
    staged_complete();
    assert(snapshot().submitted_sequence==snapshot().completed_sequence&&ready_calls==ready_before+2);
    puts("PASS NES pre-copies before TE and fills the idle half while DMA owns the other; timeout preserves mirror and LVGL ownership");
}

int main(void)
{
    lv_display_t d={.flush_cb=stage_flush,.color_format=LV_COLOR_FORMAT_RGB565,.render_mode=LV_DISPLAY_RENDER_MODE_PARTIAL,.hres=480,.vres=480};
    staged_display=&d;
    test_panel_draw=stage_draw;
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,0)==ESP_ERR_INVALID_ARG);
    assert(app_display_metrics_attach_staged_rgb565(&d,NULL,&d,19200)==ESP_ERR_INVALID_ARG);
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,7)==ESP_ERR_INVALID_ARG);
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,SIZE_MAX)==ESP_ERR_INVALID_ARG);
    d.render_mode=LV_DISPLAY_RENDER_MODE_FULL;assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,19200)==ESP_ERR_NOT_SUPPORTED);d.render_mode=LV_DISPLAY_RENDER_MODE_PARTIAL;
    d.color_format=LV_COLOR_FORMAT_RGB888;assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,19200)==ESP_ERR_NOT_SUPPORTED);d.color_format=LV_COLOR_FORMAT_RGB565;
    test_heap_fail=true;assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,19200)==ESP_ERR_NO_MEM);test_heap_fail=false;
    assert(!test_heap_live&&!semaphore_live&&d.flush_cb==stage_flush&&!d.flush_wait_cb);
    fail_semaphore=true;assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,19200)==ESP_ERR_NO_MEM);fail_semaphore=false;
    assert(!test_heap_live&&!semaphore_live&&d.flush_cb==stage_flush&&!d.flush_wait_cb);
    register_error=ESP_FAIL;assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,19200)==ESP_FAIL);register_error=ESP_OK;
    assert(!test_heap_live&&!semaphore_live&&d.flush_cb==stage_flush&&!d.flush_wait_cb);
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,&d,19200)==ESP_OK);
    assert(test_heap_live==1&&semaphore_live==1&&snapshot().staging_capacity==19200&&snapshot().staging_available);
    puts("PASS staged allocation is 64B-aligned internal DMA; mirror/sem OOM and callback failure free all temporaries with unchanged hooks");

    app_display_metrics_set_active(true);
    lv_area_t full={0,0,479,19};prepare(&d,source_a,&full);memcpy(source_snapshot,source_a,sizeof(source_a));
    fail_draw=true;staged_submit(&d,&full,source_a);fail_draw=false;
    const app_display_metrics_t failed=snapshot();
    assert(!failed.submitted_sequence&&!failed.completed_sequence&&failed.dropped_regions==1&&
           failed.staging_error==ESP_FAIL&&!atomic_load(&d.flushing)&&!atomic_load(&dma_live));
    d.flush_wait_cb(&d);
    puts("PASS synchronous panel submission failure leaves no phantom transfer or blocked LVGL buffer");
    staged_submit(&d,&full,source_a);assert(!memcmp(source_a,source_snapshot,sizeof(source_a)));
    for(size_t i=0;i<19200;i+=2)assert(dma_pixels[i]==source_a[i+1]&&dma_pixels[i+1]==source_a[i]);
    /* Rendering into the other PSRAM buffer while physical DMA reads this
     * mirror cannot change its byte stream. */
    next_area=(lv_area_t){0,0,239,39};prepare(&d,source_b,&next_area);memcpy(source_snapshot,source_b,sizeof(source_b));delay(10);
    assert(!memcmp(dma_pixels,expected,dma_bytes));
    atomic_store(&ready_gate,false);atomic_store(&release_ready,false);atomic_store(&pause_ready,true);
    pthread_t isr;assert(!pthread_create(&isr,NULL,staged_isr,NULL));wait_flag(&ready_gate);
    atomic_store(&submit_done,false);pthread_t submitter;assert(!pthread_create(&submitter,NULL,staged_submit_thread,NULL));
    wait_blocked();delay(10);assert(!atomic_load(&submit_done)&&atomic_load(&stage_calls)==1&&!memcmp(dma_pixels,expected,dma_bytes));
    atomic_store(&release_ready,true);pthread_join(isr,NULL);pthread_join(submitter,NULL);
    assert(atomic_load(&submit_done)&&atomic_load(&stage_calls)==2&&dma_bytes==19200);
    assert(!memcmp(source_b,source_snapshot,sizeof(source_b)));staged_complete();
    puts("PASS source remains unswapped; mirror is immutable through DMA/late completion publication; 240x40 fits without a 20-row limit");

    /* Old give races the next staged submission and wait, after the mirror is
     * safe to reuse but before the semaphore token itself has been sent. */
    prepare(&d,source_a,&full);staged_submit(&d,&full,source_a);
    atomic_store(&give_gate,false);atomic_store(&release_give,false);atomic_store(&pause_give,true);
    assert(!pthread_create(&isr,NULL,staged_isr,NULL));wait_flag(&give_gate);
    lv_area_t tall={2,2,3,479};prepare(&d,source_b,&tall);staged_submit(&d,&tall,source_b);assert(dma_bytes==1912);
    pthread_t waiter=launch_wait(&d);wait_blocked();atomic_store(&release_give,true);pthread_join(isr,NULL);delay(10);
    assert(!atomic_load(&waiter_done)&&atomic_load(&dma_live));staged_complete();finish_wait(waiter);
    puts("PASS narrow 2x478 packed region and old ISR token cannot prematurely release the new staged transfer");

    /* Rejecting the next chunk must first respect an older sequence whose
     * ready flag was released but whose completion publication is delayed. */
    prepare(&d,source_a,&full);staged_submit(&d,&full,source_a);
    app_display_metrics_t prior=snapshot();unsigned prior_calls=atomic_load(&stage_calls);
    atomic_store(&ready_gate,false);atomic_store(&release_ready,false);atomic_store(&pause_ready,true);
    assert(!pthread_create(&isr,NULL,staged_isr,NULL));wait_flag(&ready_gate);
    next_area=(lv_area_t){-2,0,479,19};prepare(&d,source_b,&next_area);
    atomic_store(&submit_done,false);atomic_store(&waiter_done,false);
    assert(!pthread_create(&submitter,NULL,staged_submit_thread,NULL));wait_blocked();delay(10);
    assert(!atomic_load(&submit_done)&&atomic_load(&stage_calls)==prior_calls);
    atomic_store(&release_ready,true);pthread_join(isr,NULL);pthread_join(submitter,NULL);
    app_display_metrics_t rejected=snapshot();
    assert(rejected.submitted_sequence==prior.submitted_sequence&&rejected.completed_sequence==prior.completed_sequence+1);
    assert(rejected.dropped_regions==prior.dropped_regions+1&&!atomic_load(&dma_live)&&!atomic_load(&d.flushing));
    puts("PASS invalid next chunk waits for older completion, then releases only its LVGL flag without creating a DMA sequence");

    prepare(&d,source_a,&full);
    lv_area_t bad[]={ {-2,0,479,19},{0,0,480,19},{2,0,1,19},{1,0,2,19},{0,0,479,20},{0,-2,479,19},{0,0,479,481} };
    for(unsigned i=0;i<sizeof(bad)/sizeof(*bad);++i)assert_invalid(&d,&bad[i],source_a);
    assert_invalid(&d,NULL,source_a);assert_invalid(&d,&full,NULL);
    active.header.stride+=2;assert_invalid(&d,&full,source_a);active.header.stride-=2;
    --active.header.w;assert_invalid(&d,&full,source_a);++active.header.w;
    --active.header.h;assert_invalid(&d,&full,source_a);++active.header.h;
    active.header.cf=LV_COLOR_FORMAT_RGB888;assert_invalid(&d,&full,source_a);active.header.cf=LV_COLOR_FORMAT_RGB565;
    active.data_size=19199;assert_invalid(&d,&full,source_a);active.data_size=sizeof(source_a);
    assert_invalid(&d,&full,source_a+sizeof(source_a));
    d.render_mode=LV_DISPLAY_RENDER_MODE_FULL;assert_invalid(&d,&full,source_a);d.render_mode=LV_DISPLAY_RENDER_MODE_PARTIAL;
    d.color_format=LV_COLOR_FORMAT_RGB888;assert_invalid(&d,&full,source_a);d.color_format=LV_COLOR_FORMAT_RGB565;
    lv_area_t too_big={0,0,479,21};prepare(&d,source_a,&too_big);assert_invalid(&d,&too_big,source_a);
    puts("PASS invalid bounds/alignment/format/stride/source/length safely drop without driver calls or fabricated LCD completions");

    app_display_metrics_set_active(false);app_display_metrics_set_active(true);
    uint64_t before=snapshot().submitted_sequence;
    for(unsigned i=0;i<24;++i) {
        lv_area_t a={0,(int32_t)(i*20),479,(int32_t)(i*20+19)};
        prepare(&d,source_a,&a);memcpy(source_snapshot,source_a,sizeof(source_a));d.last=i==23;
        staged_submit(&d,&a,source_a);assert(!memcmp(source_snapshot,source_a,sizeof(source_a)));staged_complete();
    }
    app_display_metrics_t m=snapshot();assert(m.submitted_sequence==before+24&&m.completed_sequence==m.submitted_sequence);
    assert(m.completed_regions==24&&m.completed_refreshes==1&&m.staging_error==ESP_OK&&m.dropped_regions);
    assert(m.staging_copied_bytes==19200ULL*29+1912&&test_heap_live==1);
    puts("PASS 24 full-width chunks preserve source bytes, count one real completed refresh and recover after dropped regions");
    test_nes_timing(&d);
    test_direct_nes(&d);
    return 0;
}
