/* Reuse the pthread/ISR scheduling endpoint harness; execute a separate
 * staged suite against a fresh production singleton in this process. */
#define main nonstaged_endpoint_example
#include "wait_test.c"
#undef main
#include "esp_heap_caps.h"
#include <string.h>

static uint8_t source_a[32000],source_b[32000],source_snapshot[32000],expected[19200];
static uint8_t *dma_pixels;
static size_t dma_bytes;
static atomic_bool dma_live,submit_done;
static atomic_uint stage_calls;
static lv_draw_buf_t active;
static lv_area_t next_area;
static lv_display_t *staged_display;
static void stage_flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{
    assert(d==staged_display&&!atomic_load(&dma_live));
    assert(!((uintptr_t)p%64)&&p!=source_a&&p!=source_b);
    dma_bytes=(size_t)(a->x2-a->x1+1)*(size_t)(a->y2-a->y1+1)*2;
    assert(dma_bytes<=sizeof(expected));
    /* Same single in-place swap as the real port. */
    for(size_t i=0;i<dma_bytes;i+=2) { uint8_t v=p[i];p[i]=p[i+1];p[i+1]=v; }
    dma_pixels=p;memcpy(expected,p,dma_bytes);atomic_store(&dma_live,true);++stage_calls;
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
int main(void)
{
    lv_display_t d={.flush_cb=stage_flush,.color_format=LV_COLOR_FORMAT_RGB565,.render_mode=LV_DISPLAY_RENDER_MODE_PARTIAL,.hres=480,.vres=480};
    staged_display=&d;
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,0)==ESP_ERR_INVALID_ARG);
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,7)==ESP_ERR_INVALID_ARG);
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,SIZE_MAX)==ESP_ERR_INVALID_ARG);
    d.render_mode=LV_DISPLAY_RENDER_MODE_FULL;assert(app_display_metrics_attach_staged_rgb565(&d,&d,19200)==ESP_ERR_NOT_SUPPORTED);d.render_mode=LV_DISPLAY_RENDER_MODE_PARTIAL;
    d.color_format=LV_COLOR_FORMAT_RGB888;assert(app_display_metrics_attach_staged_rgb565(&d,&d,19200)==ESP_ERR_NOT_SUPPORTED);d.color_format=LV_COLOR_FORMAT_RGB565;
    test_heap_fail=true;assert(app_display_metrics_attach_staged_rgb565(&d,&d,19200)==ESP_ERR_NO_MEM);test_heap_fail=false;
    assert(!test_heap_live&&!semaphore_live&&d.flush_cb==stage_flush&&!d.flush_wait_cb);
    fail_semaphore=true;assert(app_display_metrics_attach_staged_rgb565(&d,&d,19200)==ESP_ERR_NO_MEM);fail_semaphore=false;
    assert(!test_heap_live&&!semaphore_live&&d.flush_cb==stage_flush&&!d.flush_wait_cb);
    register_error=ESP_FAIL;assert(app_display_metrics_attach_staged_rgb565(&d,&d,19200)==ESP_FAIL);register_error=ESP_OK;
    assert(!test_heap_live&&!semaphore_live&&d.flush_cb==stage_flush&&!d.flush_wait_cb);
    assert(app_display_metrics_attach_staged_rgb565(&d,&d,19200)==ESP_OK);
    assert(test_heap_live==1&&semaphore_live==1&&snapshot().staging_capacity==19200&&snapshot().staging_available);
    puts("PASS staged allocation is 64B-aligned internal DMA; mirror/sem OOM and callback failure free all temporaries with unchanged hooks");

    app_display_metrics_set_active(true);
    lv_area_t full={0,0,479,19};prepare(&d,source_a,&full);memcpy(source_snapshot,source_a,sizeof(source_a));
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
    assert(m.staging_copied_bytes==19200ULL*28+1912&&test_heap_live==1);
    puts("PASS 24 full-width chunks preserve source bytes, count one real completed refresh and recover after dropped regions");
    return 0;
}
