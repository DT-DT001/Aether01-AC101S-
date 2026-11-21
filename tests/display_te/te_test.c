#include "co5300_te.h"
#include "esp_timer.h"
#include "freertos/semphr.h"
#include <assert.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <stdatomic.h>
#include <time.h>

struct test_sem { pthread_mutex_t lock; pthread_cond_t changed; bool set; };
static void (*isr)(void *);
static void *isr_context;
static bool fail_alloc;
static int fail_stage, live_sem;
static atomic_bool hold_publication, publication_entered;
static void sleep_ms(unsigned ms)
{ struct timespec t={.tv_sec=ms/1000,.tv_nsec=(long)(ms%1000)*1000000}; nanosleep(&t,NULL); }
void vTaskDelay(TickType_t ms) { sleep_ms(ms); }
int64_t esp_timer_get_time(void)
{ struct timespec t; assert(!clock_gettime(CLOCK_MONOTONIC,&t)); return (int64_t)t.tv_sec*1000000+t.tv_nsec/1000; }
SemaphoreHandle_t xSemaphoreCreateBinary(void)
{
    if(fail_alloc) return NULL;
    struct test_sem *s=calloc(1,sizeof(*s)); assert(s);
    assert(!pthread_mutex_init(&s->lock,NULL) && !pthread_cond_init(&s->changed,NULL)); ++live_sem; return s;
}
BaseType_t xSemaphoreTake(SemaphoreHandle_t s,TickType_t ms)
{
    assert(s); pthread_mutex_lock(&s->lock);
    struct timespec until; assert(!clock_gettime(CLOCK_REALTIME,&until));
    until.tv_sec+=ms/1000; until.tv_nsec+=(long)(ms%1000)*1000000;
    if(until.tv_nsec>=1000000000) { ++until.tv_sec; until.tv_nsec-=1000000000; }
    while(!s->set && ms) { int e=pthread_cond_timedwait(&s->changed,&s->lock,&until); if(e==ETIMEDOUT) break; assert(!e); }
    bool taken=s->set; s->set=false; pthread_mutex_unlock(&s->lock); return taken;
}
BaseType_t xSemaphoreGive(SemaphoreHandle_t s)
{ assert(s); pthread_mutex_lock(&s->lock); s->set=true; pthread_cond_signal(&s->changed); pthread_mutex_unlock(&s->lock); return pdTRUE; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t s,BaseType_t *wake)
{
    publication_entered=true;
    while(hold_publication) sleep_ms(1);
    *wake=pdTRUE; return xSemaphoreGive(s);
}
void vSemaphoreDelete(SemaphoreHandle_t s)
{ assert(s && live_sem); pthread_cond_destroy(&s->changed); pthread_mutex_destroy(&s->lock); free(s); --live_sem; }
esp_err_t gpio_config(const gpio_config_t *c)
{ assert(c->pin_bit_mask==(1ULL<<8) && c->intr_type==GPIO_INTR_POSEDGE && c->mode==GPIO_MODE_INPUT); return fail_stage==1 ? ESP_FAIL : ESP_OK; }
esp_err_t gpio_install_isr_service(int flags) { assert(!flags); return fail_stage==2 ? ESP_FAIL : ESP_ERR_INVALID_STATE; }
esp_err_t gpio_isr_handler_add(gpio_num_t pin,void (*callback)(void *),void *ctx)
{ assert(pin==8); if(fail_stage==3) return ESP_FAIL; isr=callback; isr_context=ctx; return ESP_OK; }
esp_err_t gpio_isr_handler_remove(gpio_num_t pin) { assert(pin==8); isr=NULL; return ESP_OK; }
esp_err_t gpio_intr_disable(gpio_num_t pin) { assert(pin==8); return ESP_OK; }
const char *esp_err_to_name(esp_err_t error) { (void)error; return "test error"; }
static void *edge_later(void *argument)
{ sleep_ms((unsigned)(uintptr_t)argument); assert(isr); isr(isr_context); return NULL; }
static void *cancel_later(void *argument)
{ sleep_ms(4); co5300_display_cancel_te(argument); return NULL; }
static void *waiting(void *argument)
{ assert(co5300_display_wait_te(argument,100)==ESP_ERR_INVALID_STATE); return NULL; }
static void *release_publication(void *argument)
{ (void)argument; sleep_ms(4); hold_publication=false; return NULL; }
static pthread_t spawn(void *(*fn)(void *),void *argument)
{ pthread_t thread; assert(!pthread_create(&thread,NULL,fn,argument)); return thread; }
static void join(pthread_t thread) { assert(!pthread_join(thread,NULL)); }
int main(void)
{
    void *io=(void *)1; co5300_te_metrics_t metrics;
    assert(co5300_display_wait_te(NULL,35)==ESP_ERR_INVALID_ARG);
    assert(co5300_te_init(io,8)==ESP_OK); isr(isr_context); /* A stale edge must not authorize a future frame. */
    pthread_t thread=spawn(edge_later,(void *)7); int64_t start=esp_timer_get_time();
    assert(co5300_display_wait_te(io,35)==ESP_OK); join(thread);
    assert(esp_timer_get_time()-start>=5000);
    assert(co5300_display_get_te_metrics(io,&metrics)==ESP_OK && metrics.edges==2 && metrics.synced==1);
    puts("PASS rising edge setup, shared GPIO ISR service, stale-token rejection and fresh-edge wait");
    start=esp_timer_get_time(); assert(co5300_display_wait_te(io,10)==ESP_ERR_TIMEOUT);
    assert(esp_timer_get_time()-start>=8000);
    start=esp_timer_get_time(); assert(co5300_display_wait_te(io,35)==ESP_ERR_TIMEOUT);
    assert(esp_timer_get_time()-start<5000);
    assert(co5300_display_get_te_metrics(io,&metrics)==ESP_OK && metrics.timeouts==1 && metrics.fallback_frames==1);
    isr(isr_context); thread=spawn(edge_later,(void *)4);
    assert(co5300_display_wait_te(io,35)==ESP_OK); join(thread);
    puts("PASS absent TE times out, cooldown keeps fallback responsive, and returning edges restore synchronization");
    thread=spawn(cancel_later,io); assert(co5300_display_wait_te(io,35)==ESP_ERR_INVALID_STATE); join(thread);
    thread=spawn(edge_later,(void *)4); assert(co5300_display_wait_te(io,35)==ESP_OK); join(thread);
    assert(co5300_display_get_te_metrics(io,&metrics)==ESP_OK && metrics.cancelled==1);
    puts("PASS sleep/reset cancellation unblocks task wait; next refresh requires a new TE edge");
    assert(co5300_display_wait_te(io,101)==ESP_ERR_INVALID_ARG);
    assert(co5300_display_get_te_metrics(io,&metrics)==ESP_OK); unsigned waits=metrics.waits;
    thread=spawn(waiting,io);
    do { sleep_ms(1); co5300_display_get_te_metrics(io,&metrics); } while(metrics.waits==waits);
    assert(co5300_display_wait_te(io,35)==ESP_ERR_INVALID_STATE);
    esp_err_t deletion=co5300_te_deinit(io); join(thread);
    assert(deletion==ESP_OK || deletion==ESP_ERR_INVALID_STATE);
    if(deletion!=ESP_OK) assert(co5300_te_deinit(io)==ESP_OK);
    assert(!live_sem && !isr);
    puts("PASS concurrent wait rejection and active-wait deletion cancellation preserve semaphore lifetime");
    assert(co5300_te_init(io,8)==ESP_OK); hold_publication=true; publication_entered=false;
    thread=spawn(edge_later,(void *)1);
    while(!publication_entered) sleep_ms(1);
    pthread_t release_thread=spawn(release_publication,NULL);
    assert(co5300_te_deinit(io)==ESP_OK); join(thread); join(release_thread);
    assert(!live_sem && !isr);
    puts("PASS already-dispatched ISR publication drains before semaphore deletion without RTOS calls under the spinlock");
    for(int stage=0;stage<=4;++stage) {
        fail_stage=stage<=3 ? stage : 0; fail_alloc=stage==4;
        esp_err_t e=co5300_te_init(io,stage==0 ? GPIO_NUM_NC : 8); assert(e!=ESP_OK);
        assert(co5300_display_get_te_metrics(io,&metrics)==ESP_OK && !metrics.available && metrics.setup_error==e);
        assert(co5300_display_wait_te(io,35)==ESP_ERR_NOT_SUPPORTED);
        assert(co5300_te_deinit(io)==ESP_OK && !live_sem && !isr);
    }
    puts("PASS absent pin and injected allocation/GPIO/service/handler failures degrade without leaking resources");
    return 0;
}
