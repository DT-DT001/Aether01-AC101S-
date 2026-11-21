/* Production flush wrapper/wait/completion with real pthread blocking and
 * explicit ISR scheduling gates. These are host timings, not panel FPS. */
#include "app_display_metrics.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>

struct test_semaphore { pthread_mutex_t lock;pthread_cond_t ready;bool token;unsigned waiters; };
static esp_lcd_panel_io_callbacks_t registered;
static void *registered_context;
static bool fail_semaphore;
static esp_err_t register_error;
static unsigned semaphore_live;
static atomic_uint ready_calls,take_calls,yield_calls,submit_calls;
static atomic_bool waiter_done;
static atomic_bool pause_ready,ready_gate,release_ready;
static atomic_bool pause_give,give_gate,release_give;
static uint8_t pixels[480*20*2];
static const lv_area_t area={0,0,479,19};
static void delay(unsigned ms) { struct timespec t={ms/1000,(long)(ms%1000)*1000000};nanosleep(&t,NULL); }
int64_t esp_timer_get_time(void) { struct timespec t;clock_gettime(CLOCK_MONOTONIC,&t);return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{ if(fail_semaphore)return NULL;SemaphoreHandle_t s=calloc(1,sizeof(*s));assert(s);++semaphore_live;assert(!pthread_mutex_init(&s->lock,NULL));assert(!pthread_cond_init(&s->ready,NULL));return s; }
void vSemaphoreDelete(SemaphoreHandle_t s) { --semaphore_live;pthread_cond_destroy(&s->ready);pthread_mutex_destroy(&s->lock);free(s); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ticks)
{
    ++take_calls;pthread_mutex_lock(&s->lock);++s->waiters;
    struct timespec until;clock_gettime(CLOCK_REALTIME,&until);until.tv_nsec+=(long)ticks*1000000;
    until.tv_sec+=until.tv_nsec/1000000000;until.tv_nsec%=1000000000;
    int result=0;while(!s->token&&result!=ETIMEDOUT) { result=pthread_cond_timedwait(&s->ready,&s->lock,&until);assert(!result||result==ETIMEDOUT); }
    bool token=s->token;s->token=false;--s->waiters;pthread_mutex_unlock(&s->lock);return token?pdTRUE:pdFALSE;
}
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *woken)
{
    if(atomic_exchange(&pause_give,false)) { atomic_store(&give_gate,true);while(!atomic_load(&release_give))delay(1); }
    pthread_mutex_lock(&s->lock);bool added=!s->token;s->token=true;
    *woken=added&&s->waiters?pdTRUE:pdFALSE;pthread_cond_signal(&s->ready);pthread_mutex_unlock(&s->lock);return added?pdTRUE:pdFALSE;
}
void test_yield_from_isr(void) { ++yield_calls; }
void lv_display_set_flush_cb(lv_display_t *d,lv_display_flush_cb_t cb) { d->flush_cb=cb; }
void lv_display_set_flush_wait_cb(lv_display_t *d,lv_display_flush_wait_cb_t cb) { d->flush_wait_cb=cb; }
bool lv_display_flush_is_last(lv_display_t *d) { return atomic_load(&d->last); }
void lvgl_port_flush_ready(lv_display_t *d)
{
    assert(d==registered_context);atomic_store(&d->flushing,false);++ready_calls;
    if(atomic_exchange(&pause_ready,false)) { atomic_store(&ready_gate,true);while(!atomic_load(&release_ready))delay(1); }
}
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,const esp_lcd_panel_io_callbacks_t *cb,void *ctx)
{ assert(io&&cb&&ctx);if(register_error)return register_error;registered=*cb;registered_context=ctx;return ESP_OK; }
static void original_flush(lv_display_t *d,const lv_area_t *a,uint8_t *p)
{ assert(d==registered_context&&a==&area&&p==pixels&&atomic_load(&d->flushing));++submit_calls; }
static app_display_metrics_t snapshot(void) { app_display_metrics_t m;app_display_metrics_get(&m);return m; }
static void submit(lv_display_t *d,bool last)
{ assert(!atomic_exchange(&d->flushing,true));atomic_store(&d->last,last);d->flush_cb(d,&area,pixels); }
static void complete(void)
{ registered.on_color_trans_done(registered_context,NULL,registered_context); }
static void *completion_thread(void *unused) { (void)unused;complete();return NULL; }
static void *wait_thread(void *context)
{
    lv_display_t *d=context;uint64_t target=snapshot().submitted_sequence;
    d->flush_wait_cb(d);assert(snapshot().completed_sequence>=target);
    /* Same operation as LVGL 9.5 wait_for_flushing after callback returns. */
    atomic_store(&d->flushing,false);atomic_store(&waiter_done,true);return NULL;
}
static pthread_t launch_wait(lv_display_t *d)
{ pthread_t thread;atomic_store(&waiter_done,false);assert(!pthread_create(&thread,NULL,wait_thread,d));return thread; }
static void wait_flag(atomic_bool *flag)
{ for(unsigned i=0;i<2000&&!atomic_load(flag);++i)delay(1);assert(atomic_load(flag)); }
static void wait_blocked(void)
{ for(unsigned i=0;i<2000&&!snapshot().flush_waiting;++i)delay(1);assert(snapshot().flush_waiting&&!atomic_load(&waiter_done)); }
static void finish_wait(pthread_t thread)
{ pthread_join(thread,NULL);assert(atomic_load(&waiter_done));app_display_metrics_t m=snapshot();assert(m.completed_sequence==m.submitted_sequence&&!m.flush_waiting&&!m.flush_stalled); }
int main(void)
{
    lv_display_t d={.flush_cb=original_flush};assert(app_display_metrics_attach(&d,&d)==ESP_OK);
    app_display_metrics_set_active(true);
    submit(&d,true);complete();unsigned takes=atomic_load(&take_calls);
    d.flush_wait_cb(&d);assert(atomic_load(&take_calls)==takes);
    puts("PASS DMA done before wait returns immediately from sequence; no required semaphore token consumption");

    submit(&d,true);pthread_t waiter=launch_wait(&d);wait_blocked();delay(10);
    assert(!atomic_load(&waiter_done)&&atomic_load(&d.flushing));complete();finish_wait(waiter);
    puts("PASS stale binary token from previous chunk cannot release a new DMA-owned buffer");

    /* Old ISR clears LVGL's flag, allowing the next chunk on the other core,
     * but has not yet published its completed sequence or semaphore token. */
    submit(&d,false);atomic_store(&ready_gate,false);atomic_store(&release_ready,false);atomic_store(&pause_ready,true);
    pthread_t isr;assert(!pthread_create(&isr,NULL,completion_thread,NULL));wait_flag(&ready_gate);
    submit(&d,true);waiter=launch_wait(&d);wait_blocked();atomic_store(&release_ready,true);pthread_join(isr,NULL);delay(10);
    app_display_metrics_t m=snapshot();assert(m.submitted_sequence==4&&m.completed_sequence==3&&!atomic_load(&waiter_done)&&atomic_load(&d.flushing));
    complete();finish_wait(waiter);
    puts("PASS cross-core ready-before-completion publication cannot advance the following chunk's sequence");

    /* More subtle case: old completion is already published, but its give
     * itself is delayed until the next chunk has begun waiting. */
    submit(&d,false);atomic_store(&give_gate,false);atomic_store(&release_give,false);atomic_store(&pause_give,true);
    assert(!pthread_create(&isr,NULL,completion_thread,NULL));wait_flag(&give_gate);
    submit(&d,true);waiter=launch_wait(&d);wait_blocked();atomic_store(&release_give,true);pthread_join(isr,NULL);delay(10);
    m=snapshot();assert(m.submitted_sequence==6&&m.completed_sequence==5&&!atomic_load(&waiter_done)&&atomic_load(&d.flushing));
    complete();finish_wait(waiter);
    puts("PASS late old ISR give after next submit/wait is only a wake hint, never permission to reuse that buffer");

    submit(&d,true);waiter=launch_wait(&d);wait_blocked();
    for(unsigned i=0;i<2000&&!snapshot().flush_stalled;++i)delay(1);
    m=snapshot();assert(m.flush_stalled&&m.flush_wait_timeouts&&!atomic_load(&waiter_done)&&atomic_load(&d.flushing));
    complete();finish_wait(waiter);assert(snapshot().max_flush_wait_us>=100000);
    puts("PASS 100ms missing-completion timeout reports stall while retaining DMA ownership; real completion recovers");

    app_display_metrics_set_active(false);uint64_t before=snapshot().submitted_sequence;
    app_display_metrics_set_active(true);
    for(unsigned i=0;i<24;++i) {
        submit(&d,i==23);
        if(i&1) { waiter=launch_wait(&d);wait_blocked();complete();finish_wait(waiter); }
        else { complete();d.flush_wait_cb(&d); }
    }
    m=snapshot();assert(m.submitted_sequence==before+24&&m.completed_sequence==m.submitted_sequence);
    assert(m.completed_regions==24&&m.completed_refreshes==1&&atomic_load(&ready_calls)==atomic_load(&submit_calls));
    assert(atomic_load(&yield_calls)>0);
    puts("PASS 24 DMA chunks retain one final-refresh count, lifetime sequences survive media reset, and ISR wakes explicitly yield");
    return 0;
}
