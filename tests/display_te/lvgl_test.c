#include "app_display_sync.h"
#include "app_display_metrics.h"
#include "co5300_display.h"
#include "esp_timer.h"
#include <assert.h>
#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#ifdef AETHER_VIDEO_DIAGNOSTIC
#include "freertos/task.h"
#endif

static unsigned waits,flushes,cycles,first_waits;
static esp_err_t wait_error;
static int64_t now_us;
static bool profiling, nested_wait;
static unsigned profile_logs;
static bool polling, reentering_log;
static unsigned frequency_logs, fallback_logs;
static uint32_t period_us = 16667;
#ifdef AETHER_VIDEO_DIAGNOSTIC
static BaseType_t mock_core, mock_affinity=tskNO_AFFINITY;
static unsigned placement_logs;
static int placement_last, placement_affinity;
static unsigned long placement_mask, placement_samples;
BaseType_t xPortGetCoreID(void) { assert(!polling); return mock_core; }
BaseType_t xTaskGetCoreID(void *task) { assert(!polling && !task); return mock_affinity; }
#endif
#ifndef TEST_BUFFER_LINES
#define TEST_BUFFER_LINES 40
#endif
#define FULL_TRANSFERS (480 / TEST_BUFFER_LINES)
static uint16_t buffer1[480*TEST_BUFFER_LINES],buffer2[480*TEST_BUFFER_LINES];
int64_t esp_timer_get_time(void) { return now_us; }
void app_display_metrics_nes_reset(void) {}
void app_display_metrics_nes_arm(const lv_area_t *area, int64_t edge_us,
                                 uint32_t period, uint16_t panel_rows)
{ assert(!area&&!edge_us&&!period&&!panel_rows); }
esp_err_t app_display_metrics_nes_write(lv_display_t *display, const lv_area_t *area,
    const uint16_t *pixels, app_display_nes_wait_te_fn wait_te,
    app_display_nes_strip_fn write_strip, void *ctx)
{
    (void)display; (void)area; (void)pixels; (void)wait_te; (void)write_strip; (void)ctx;
    assert(!"normal refresh must not access the NES DMA writer");
    return ESP_FAIL;
}
void test_display_log(const char *tag, const char *format, ...)
{
    if (!strcmp(tag, "display_sync")) {
        /* Ordinary rendering and TE failure must never enter stdout. The
         * reporter may reenter without holding its cache lock while logging. */
        assert(polling);
        if (!strncmp(format, "TE GPIO", 7)) {
            char line[128];
            va_list arguments; va_start(arguments, format);
            vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
            assert(!strcmp(line, "TE GPIO8 frequency=59.99 Hz"));
            ++frequency_logs;
        }
#ifdef AETHER_VIDEO_DIAGNOSTIC
        else if (!strncmp(format, "GUI placement", 13)) {
            char line[128];
            va_list arguments; va_start(arguments, format);
            vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
            assert(sscanf(line, "GUI placement last_core=%d coremask=0x%lx affinity=%d samples=%lu",
                &placement_last, &placement_mask, &placement_affinity, &placement_samples)==4);
            assert(placement_samples && placement_last==mock_core);
            assert(placement_affinity==(mock_affinity==tskNO_AFFINITY ? -1 : mock_affinity));
            ++placement_logs;
        }
#endif
        else {
            assert(!strcmp(format, "TE unavailable (%s), bounded unsynchronized fallback"));
            ++fallback_logs;
        }
        if (!reentering_log) {
            reentering_log = true;
            app_display_sync_poll_diagnostics((void *)1);
            reentering_log = false;
        }
        return;
    }
    if (strcmp(tag, "display_perf") || !profiling) return;
    char line[768];
    va_list arguments; va_start(arguments, format);
    vsnprintf(line, sizeof(line), format, arguments); va_end(arguments);
    unsigned long frames, chunks;
    unsigned long long span, fps, period, pixels, render, te, flush_us, dma, rest, maximum, outside;
    assert(sscanf(line, "active_us=%llu frames=%lu start_fps_x100=%llu period_us=%llu chunks=%lu pixels=%llu render_us=%llu te_us=%llu flushcb_us=%llu dmawait_us=%llu rest_wall_us=%llu max_render_us=%llu outside_dma_us=%llu",
        &span, &frames, &fps, &period, &chunks, &pixels, &render, &te, &flush_us,
        &dma, &rest, &maximum, &outside) == 13);
    assert(span >= 1500000 && frames > 1 && fps && period);
    assert(chunks == frames * FULL_TRANSFERS && pixels == frames * 480ULL * 480);
    assert(te == frames * 2000 && flush_us == chunks * 300 && rest == frames * 1000);
    assert(render == te + flush_us + dma + rest && outside == 0);
    const unsigned long long expected_dma = chunks * (700ULL + (nested_wait ? 900 : 0));
    assert(dma == expected_dma || (!nested_wait && dma + 700 == expected_dma));
    assert(maximum == 3000 + FULL_TRANSFERS * (300 + 700 + (nested_wait ? 900 : 0)));
    const unsigned long long start_span = render - maximum + (frames - 1) * 10000;
    assert(period == start_span / (frames - 1));
    assert(fps == (frames - 1) * 100000000ULL / start_span);
    ++profile_logs;
}
const char *esp_err_to_name(esp_err_t error) { (void)error; return "mock TE fallback"; }
esp_err_t co5300_display_wait_te(esp_lcd_panel_io_handle_t io,uint32_t timeout)
{ assert(io==(void *)1 && timeout==35); ++waits; if(profiling) now_us+=2000; return wait_error; }
esp_err_t co5300_display_get_te_metrics(esp_lcd_panel_io_handle_t io,co5300_te_metrics_t *metrics)
{ assert(io==(void *)1); *metrics=(co5300_te_metrics_t){.pin=8,.available=true,.edges=waits,.synced=waits,.last_period_us=period_us}; return ESP_OK; }
static void poll_diagnostics(void)
{ polling=true; app_display_sync_poll_diagnostics((void *)1); polling=false; }
static void round_event(lv_event_t *event) { app_display_rounder(lv_event_get_param(event)); }
static void observe(lv_event_t *event)
{
    if(lv_event_get_code(event)==LV_EVENT_RENDER_START) { ++cycles; first_waits=waits; if(profiling) now_us+=1000; }
    if(lv_event_get_code(event)==LV_EVENT_RENDER_READY) assert(waits==first_waits+1);
}
static void flush(lv_display_t *display,const lv_area_t *a,uint8_t *pixels)
{
    assert(a->x1>=0 && a->y1>=0 && a->x2<480 && a->y2<480);
    assert(!(a->x1&1) && !(a->y1&1) && (a->x2&1) && (a->y2&1));
    unsigned w=(unsigned)lv_area_get_width(a),h=(unsigned)lv_area_get_height(a);
    lv_draw_buf_t *buf=lv_display_get_buf_active(display);
    assert(buf && buf->header.stride==w*2 && w*h*2<=buf->data_size);
    assert(waits==first_waits+1); /* Every partial transfer shares the first TE authorization. */
    volatile uint8_t first=pixels[0],last=pixels[w*h*2-1]; (void)first; (void)last;
    ++flushes;
    if(profiling) {
        if(nested_wait) {
            lv_display_send_event(display,LV_EVENT_FLUSH_WAIT_START,NULL);
            now_us+=900;
            lv_display_send_event(display,LV_EVENT_FLUSH_WAIT_FINISH,NULL);
        }
        now_us+=300;
    } else lv_display_flush_ready(display);
}
static void dma_wait(lv_display_t *display)
{ assert(profiling); now_us+=700; lv_display_flush_ready(display); }
static void refresh(lv_obj_t *object)
{ lv_obj_invalidate(object); now_us+=10000; lv_tick_inc(10); lv_refr_now(NULL); }
int main(void)
{
    lv_init(); lv_display_t *display=lv_display_create(480,480); assert(display);
    lv_display_set_color_format(display,LV_COLOR_FORMAT_RGB565);
    lv_display_set_buffers(display,buffer1,buffer2,sizeof(buffer1),LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(display,flush);
    lv_display_add_event_cb(display,round_event,LV_EVENT_INVALIDATE_AREA,NULL);
    assert(app_display_sync_attach(display,(void *)1)==ESP_OK);
    lv_display_add_event_cb(display,observe,LV_EVENT_ALL,NULL);
    refresh(lv_screen_active()); assert(flushes==FULL_TRANSFERS && cycles==1 && waits==1);
    assert(!frequency_logs && !fallback_logs);
    poll_diagnostics(); assert(frequency_logs==1 && !fallback_logs);
    poll_diagnostics(); assert(frequency_logs==1);
    now_us+=9999999; poll_diagnostics(); assert(frequency_logs==1);
    ++now_us; poll_diagnostics(); assert(frequency_logs==2);
#ifdef AETHER_VIDEO_DIAGNOSTIC
    assert(placement_logs==2 && placement_last==0 && placement_mask==1 && placement_affinity==-1 && placement_samples==1);
    /* Model a first coprocessor use pinning the owner after its first draw.
     * The real next RENDER_READY must publish the new placement. */
    mock_core=mock_affinity=1; refresh(lv_screen_active());
#endif
    unsigned n=waits; lv_refr_now(NULL); assert(waits==n); /* No dirty region has no TE wait. */
    printf("PASS real LVGL double-buffer %d-line refresh: %d transfers, one TE wait before the first chunk\n", TEST_BUFFER_LINES, FULL_TRANSFERS);
    lv_obj_t *object=lv_obj_create(lv_screen_active()); lv_obj_remove_style_all(object);
    lv_obj_set_style_bg_color(object,lv_color_hex(0xA05020),0); lv_obj_set_style_bg_opa(object,LV_OPA_COVER,0);
    /* Width 256 would fit 75 rows without get_max_row's rounder event. */
    lv_obj_set_pos(object,11,13); lv_obj_set_size(object,255,301); refresh(object);
    for(unsigned i=0;i<1000;++i) {
        lv_obj_set_pos(object,(int32_t)(i*71%460),(int32_t)(i*37%460));
        lv_obj_set_size(object,(int32_t)(i*43%259+1),(int32_t)(i*19%309+1));
        refresh(object);
    }
    assert(cycles==waits);
    puts("PASS 1000 odd/small/clipped regions and narrow tall chunks: even starts/sizes, packed stride and bounded pixel length");
    wait_error=ESP_ERR_TIMEOUT; n=flushes; refresh(lv_screen_active()); assert(flushes==n+FULL_TRANSFERS);
    assert(!fallback_logs); poll_diagnostics(); assert(fallback_logs==1);
#ifdef AETHER_VIDEO_DIAGNOSTIC
    assert(placement_logs==3 && placement_last==1 && placement_mask==3 && placement_affinity==1 && placement_samples==cycles);
#endif
    wait_error=ESP_ERR_INVALID_STATE; n=flushes; refresh(lv_screen_active()); assert(flushes==n+FULL_TRANSFERS);
    poll_diagnostics(); assert(fallback_logs==1);
    now_us+=2000000; poll_diagnostics(); assert(fallback_logs==2);
    wait_error=ESP_OK; n=flushes; refresh(lv_screen_active()); assert(flushes==n+FULL_TRANSFERS && waits==cycles);
    unsigned frequencies=frequency_logs;
    period_us=0; now_us+=10000000; poll_diagnostics(); assert(frequency_logs==frequencies);
    period_us=16667; poll_diagnostics(); assert(frequency_logs==frequencies+1);
    puts("PASS real LVGL callback logs no normal frequency/fallback messages; background reporter preserves Hz, 10s/2s limits and ignores sleeping TE periods");
    /* Actual LVGL render/flush/wait ordering with deterministic wall costs.
     * The final DMA is left pending until the next real wait callback. */
    now_us+=1000000; profiling=true;
    lv_display_set_flush_wait_cb(display,dma_wait);
    for(unsigned i=0;i<100;++i) refresh(lv_screen_active());
    assert(profile_logs);
    unsigned logged=profile_logs;
    now_us+=10000000; n=waits; lv_refr_now(NULL);
    assert(profile_logs==logged && waits==n); /* Idle emits no profiling row. */
    refresh(lv_screen_active()); assert(profile_logs==logged); /* Idle time was discarded. */
    now_us+=1000000; nested_wait=true;
    for(unsigned i=0;i<100;++i) refresh(lv_screen_active());
    assert(profile_logs>logged);
    puts("PASS real LVGL phase accounting: TE excluded from flush, nested DMA excluded once, exact pixels/chunks/rest, idle windows discarded");
    /* A pending callback error cannot survive deletion, even if the IO handle
     * is later reused by a replacement GUI display. */
    wait_error=ESP_ERR_TIMEOUT; refresh(lv_screen_active());
    assert(lv_mem_test()==LV_RESULT_OK); lv_display_delete(display); assert(lv_mem_test()==LV_RESULT_OK);
    frequencies=frequency_logs; unsigned errors=fallback_logs;
    now_us+=10000000; poll_diagnostics(); assert(frequency_logs==frequencies && fallback_logs==errors);
    display=lv_display_create(480,480); assert(display);
    assert(app_display_sync_attach(display,(void *)1)==ESP_OK);
#ifdef AETHER_VIDEO_DIAGNOSTIC
    const unsigned placements=placement_logs;
#endif
    poll_diagnostics(); assert(frequency_logs==frequencies+1 && fallback_logs==errors);
#ifdef AETHER_VIDEO_DIAGNOSTIC
    assert(placement_logs==placements); /* No old display's placement leaks. */
    puts("PASS diagnostic-only post-render placement observes both cores and changed affinity; all placement logging remains in the background poll");
#endif
    lv_display_delete(display); lv_deinit();
    puts("PASS diagnostic snapshots retain no GUI allocation and delete/reattach discards the old pending error");
    puts("PASS TE timeout/cancel/wake fallback does not strand LVGL flush ownership or repeat waits per chunk");
    return 0;
}
