#include "app_display_metrics.h"
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "freertos/semphr.h"

static int64_t now_us;
static unsigned ready_count;
static esp_err_t register_error;
static esp_lcd_panel_io_callbacks_t registered;
static void *registered_context;
static bool fail_semaphore;
static unsigned allocated,deleted,submits;
struct test_semaphore { bool token; };
SemaphoreHandle_t xSemaphoreCreateBinary(void) { if(fail_semaphore)return NULL;++allocated;return calloc(1,sizeof(struct test_semaphore)); }
void vSemaphoreDelete(SemaphoreHandle_t signal) { ++deleted;free(signal); }
BaseType_t xSemaphoreTake(SemaphoreHandle_t signal,TickType_t ticks) { (void)ticks;bool token=signal->token;signal->token=false;return token; }
BaseType_t xSemaphoreGiveFromISR(SemaphoreHandle_t signal,BaseType_t *woken) { signal->token=true;*woken=pdFALSE;return pdTRUE; }
void test_yield_from_isr(void) { assert(!"no task is waiting in deterministic counter test"); }
void lv_display_set_flush_cb(lv_display_t *d,lv_display_flush_cb_t cb) { d->flush_cb=cb; }
void lv_display_set_flush_wait_cb(lv_display_t *d,lv_display_flush_wait_cb_t cb) { d->flush_wait_cb=cb; }
static void original_flush(lv_display_t *d,const lv_area_t *area,uint8_t *pixels) { (void)d;(void)area;(void)pixels;++submits; }
static void submit(lv_display_t *d) { d->flushing=true;d->flush_cb(d,NULL,NULL); }
int64_t esp_timer_get_time(void) { return now_us; }
bool lv_display_flush_is_last(lv_display_t *display) { return display->last; }
void lvgl_port_flush_ready(lv_display_t *display) { assert(display == registered_context);display->flushing=false;++ready_count; }
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t io,
    const esp_lcd_panel_io_callbacks_t *callbacks, void *context)
{
    assert(io && callbacks && context);
    if (!register_error) { registered = *callbacks; registered_context = context; }
    return register_error;
}

int main(void)
{
    lv_display_t display = {.flush_cb=original_flush};
    void *io = &display;
    app_display_metrics_t metrics;
    assert(app_display_metrics_attach(NULL, &display) == ESP_ERR_INVALID_ARG);
    assert(app_display_metrics_attach(io, NULL) == ESP_ERR_INVALID_ARG);
    fail_semaphore=true;assert(app_display_metrics_attach(io,&display)==ESP_ERR_NO_MEM);
    assert(display.flush_cb==original_flush&&!display.flush_wait_cb&&!allocated);
    fail_semaphore=false;
    register_error = ESP_FAIL;
    assert(app_display_metrics_attach(io, &display) == ESP_FAIL);
    assert(allocated==1&&deleted==1&&display.flush_cb==original_flush&&!display.flush_wait_cb);
    app_display_metrics_get(&metrics); assert(!metrics.available);
    register_error = ESP_OK;
    assert(app_display_metrics_attach(io, &display) == ESP_OK);
    assert(display.flush_cb!=original_flush&&display.flush_wait_cb);
    assert(app_display_metrics_attach(io,&display)==ESP_ERR_INVALID_STATE);
    submit(&display);
    assert(!registered.on_color_trans_done(io, NULL, registered_context));
    app_display_metrics_get(&metrics);
    assert(ready_count == 1 && metrics.available && !metrics.completed_regions);
    puts("PASS callback registration/failure and inactive forwarding preserve flush-ready");

    now_us = 100000;
    app_display_metrics_set_active(true);
    for (unsigned frame = 1; frame <= 30; ++frame) {
        now_us = 100000 + (int64_t)frame * 1000000 / 30;
        for (unsigned region = 0; region < 12; ++region) {
            display.last = region == 11;
            submit(&display);
            assert(!registered.on_color_trans_done(io, NULL, registered_context));
        }
    }
    app_display_metrics_get(&metrics);
    assert(metrics.completed_regions == 360 && metrics.completed_refreshes == 30);
    assert(metrics.refresh_fps_x100 == 3000 && ready_count == 361);
    assert(metrics.max_refresh_gap_us >= 33333 && metrics.max_refresh_gap_us <= 33334);
    puts("PASS 12 partial transfers count as one completed refresh; synthetic 30Hz timestamps produce correct counters");

    now_us += 1500000;
    app_display_metrics_get(&metrics); assert(!metrics.refresh_fps_x100);
    app_display_metrics_set_active(false);
    app_display_metrics_get(&metrics); assert(!metrics.active && !metrics.completed_refreshes);
    app_display_metrics_set_active(true);
    for (unsigned frame = 1; frame <= 10000; ++frame) {
        now_us += 16667; display.last = true;
        submit(&display);
        registered.on_color_trans_done(io, NULL, registered_context);
    }
    app_display_metrics_get(&metrics);
    assert(metrics.completed_refreshes == 10000 && metrics.completed_regions == 10000);
    assert(metrics.refresh_fps_x100 >= 5999 && metrics.refresh_fps_x100 <= 6000);
    assert(metrics.max_refresh_gap_us == 16667);
    assert(metrics.flush_wait_available&&metrics.submitted_sequence==10361&&metrics.completed_sequence==10361&&submits==10361);
    display.flush_wait_cb(&display);
    app_display_metrics_get(&metrics);assert(metrics.flush_wait_calls==1&&!metrics.flush_waiting&&!metrics.flush_stalled);
    app_display_metrics_get(NULL);
    puts("PASS stale cadence clears and 10000 refreshes retain bounded, accurate counters");
    return 0;
}
