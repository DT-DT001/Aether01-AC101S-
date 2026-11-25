#include "app_display_sync.h"
#include "app_display_metrics.h"
#include <stdlib.h>
#include <inttypes.h>
#include "co5300_display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "src/display/lv_display_private.h"
#ifdef AETHER_VIDEO_DIAGNOSTIC
#include "freertos/task.h"
#endif

typedef struct {
    esp_lcd_panel_io_handle_t io;
    bool first_flush;
    uint32_t diagnostics_generation;
    /* All profiling state belongs to the LVGL task. These are wall times,
     * including preemption; final DMA completion can follow RENDER_READY. */
    bool rendering, wait_in_render;
    unsigned flush_depth, wait_depth;
    int64_t render_begin_us, flush_begin_us, wait_begin_us, last_render_end_us;
    int64_t window_begin_us, window_first_start_us;
    uint64_t frame_te_us, frame_flush_us, frame_wait_us, flush_nested_wait_us;
    uint64_t frame_pixels, window_pixels;
    uint32_t frame_chunks, window_frames, window_chunks;
    uint64_t window_render_us, window_te_us, window_flush_us, window_wait_us;
    uint64_t window_outside_wait_us, window_rest_us, window_max_render_us;
} display_sync_t;

/* The panel supports one TE source. Copy only POD diagnostic state across
 * tasks: telemetry must never retain a GUI-owned display_sync_t pointer. */
static portMUX_TYPE s_diagnostics_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    esp_lcd_panel_io_handle_t io;
    uint32_t generation;
    bool error_pending;
    esp_err_t error;
    int64_t error_after_us, frequency_after_us;
#ifdef AETHER_VIDEO_DIAGNOSTIC
    bool placement_sampled;
    BaseType_t last_core, affinity;
    uint32_t core_mask, placement_samples;
#endif
} s_diagnostics;

/* The active page configures this on the LVGL task. */
static lv_display_t *s_nes_display;
static lv_area_t s_nes_area;

void app_display_sync_set_nes_viewport(lv_display_t *display, const lv_area_t *area)
{
    s_nes_display = area ? display : NULL;
    if (area) {
        s_nes_area = *area;
        app_display_metrics_nes_reset();
    }
    app_display_metrics_nes_arm(NULL, 0, 0, 0);
}

static void arm_nes_te(lv_display_t *display, esp_lcd_panel_io_handle_t io,
                       int64_t began, int64_t returned)
{
    co5300_te_metrics_t te;
    if (co5300_display_get_te_metrics(io, &te) == ESP_OK &&
        te.last_edge_us >= began && te.last_edge_us <= returned) {
        app_display_metrics_nes_arm(&s_nes_area, te.last_edge_us, te.last_period_us,
            (uint16_t)lv_display_get_vertical_resolution(display));
    } else {
        app_display_metrics_nes_arm(NULL, 0, 0, 0);
    }
}

static esp_err_t wait_nes_te(void *ctx)
{
    esp_lcd_panel_io_handle_t io = ctx;
    const int64_t began = esp_timer_get_time();
    const esp_err_t error = co5300_display_wait_te(io, 35);
    if (error == ESP_OK) arm_nes_te(s_nes_display, io, began, esp_timer_get_time());
    return error;
}

static esp_err_t write_nes_strip(void *ctx, const lv_area_t *first_window,
                                 const void *pixels, size_t bytes)
{
    if (!first_window) return co5300_display_write_rgb565_strip(ctx, NULL, pixels, bytes);
    const co5300_window_t window = {
        first_window->x1, first_window->y1, first_window->x2 + 1, first_window->y2 + 1,
    };
    return co5300_display_write_rgb565_strip(ctx, &window, pixels, bytes);
}

static bool has_visible_child(lv_obj_t *parent, lv_obj_t *except)
{
    if (!parent) return false;
    const uint32_t count = lv_obj_get_child_count(parent);
    for (uint32_t i = 0; i < count; ++i) {
        lv_obj_t *child = lv_obj_get_child(parent, (int32_t)i);
        if (child != except && !lv_obj_has_flag(child, LV_OBJ_FLAG_HIDDEN)) return true;
    }
    return false;
}

bool app_display_sync_present_nes(lv_obj_t *image, const uint16_t *pixels,
                                  uint16_t width, uint16_t height)
{
    if (!image || !s_nes_display) return false;
    lv_display_t *display = lv_obj_get_display(image);
    if (display != s_nes_display || display->inv_p || display->rendering_in_progress ||
        display->offset_x || display->offset_y || lv_display_get_rotation(display) != LV_DISPLAY_ROTATION_0 ||
        lv_display_get_screen_prev(display) || lv_display_get_screen_loading(display))
        return false;
    lv_obj_t *container = lv_obj_get_parent(image);
    lv_obj_t *screen = lv_display_get_screen_active(display);
    if (!container || !screen || lv_obj_get_parent(container) != screen ||
        lv_obj_has_flag(container, LV_OBJ_FLAG_HIDDEN) || lv_obj_has_flag(image, LV_OBJ_FLAG_HIDDEN) ||
        has_visible_child(container, image) || has_visible_child(screen, container) ||
        has_visible_child(lv_display_get_layer_top(display), NULL) ||
        has_visible_child(lv_display_get_layer_sys(display), NULL))
        return false;

    lv_area_t coords;
    lv_obj_get_coords(image, &coords);
    if (coords.x1 != s_nes_area.x1 || coords.x2 != s_nes_area.x2 ||
        coords.y1 != s_nes_area.y1 || coords.y2 != s_nes_area.y2 ||
        width != lv_area_get_width(&coords) || height != lv_area_get_height(&coords))
        return false;

    portENTER_CRITICAL(&s_diagnostics_lock);
    esp_lcd_panel_io_handle_t io = s_diagnostics.io;
    portEXIT_CRITICAL(&s_diagnostics_lock);
    return io && app_display_metrics_nes_write(display, &coords, pixels,
        wait_nes_te, write_nes_strip, io) == ESP_OK;
}

void app_display_sync_poll_diagnostics(esp_lcd_panel_io_handle_t io)
{
    if (!io) return;
    portENTER_CRITICAL(&s_diagnostics_lock);
    const uint32_t generation = s_diagnostics.generation;
    const bool attached = io == s_diagnostics.io;
    portEXIT_CRITICAL(&s_diagnostics_lock);
    if (!attached) return;

    /* get_te_metrics takes its own short lock and never accesses LVGL. Do not
     * nest that lock with ours or keep either lock across console output. */
    co5300_te_metrics_t metrics;
    const bool measured = co5300_display_get_te_metrics(io, &metrics) == ESP_OK &&
        metrics.available && metrics.last_period_us;
    const int64_t now = esp_timer_get_time();
    bool report_error = false, report_frequency = false;
    esp_err_t error = ESP_OK;
#ifdef AETHER_VIDEO_DIAGNOSTIC
    bool report_placement = false;
    BaseType_t last_core = 0, affinity = 0;
    uint32_t core_mask = 0, placement_samples = 0;
#endif
    portENTER_CRITICAL(&s_diagnostics_lock);
    if (io == s_diagnostics.io && generation == s_diagnostics.generation) {
        if (s_diagnostics.error_pending && now >= s_diagnostics.error_after_us) {
            report_error = true;
            error = s_diagnostics.error;
            s_diagnostics.error_pending = false;
            s_diagnostics.error_after_us = now + 2000000;
        }
        if (measured && now >= s_diagnostics.frequency_after_us) {
            report_frequency = true;
            s_diagnostics.frequency_after_us = now + 10000000;
#ifdef AETHER_VIDEO_DIAGNOSTIC
            report_placement = s_diagnostics.placement_sampled;
            last_core = s_diagnostics.last_core;
            affinity = s_diagnostics.affinity;
            core_mask = s_diagnostics.core_mask;
            placement_samples = s_diagnostics.placement_samples;
#endif
        }
    }
    portEXIT_CRITICAL(&s_diagnostics_lock);
    if (report_error) {
        ESP_LOGW("display_sync", "TE unavailable (%s), bounded unsynchronized fallback", esp_err_to_name(error));
    }
    if (report_frequency) {
        const uint32_t hz_x100 = (uint32_t)(100000000ULL / metrics.last_period_us);
        ESP_LOGI("display_sync", "TE GPIO%d frequency=%lu.%02lu Hz", metrics.pin,
            (unsigned long)(hz_x100 / 100), (unsigned long)(hz_x100 % 100));
    }
#ifdef AETHER_VIDEO_DIAGNOSTIC
    if (report_placement) {
        ESP_LOGI("display_sync", "GUI placement last_core=%d coremask=0x%lx affinity=%d samples=%lu",
            (int)last_core, (unsigned long)core_mask,
            affinity == tskNO_AFFINITY ? -1 : (int)affinity, (unsigned long)placement_samples);
    }
#endif
}

static uint64_t elapsed_us(int64_t began, int64_t now)
{ return now > began ? (uint64_t)(now - began) : 0; }

static void profile_begin(display_sync_t *sync, int64_t now)
{
    if (!sync->window_frames || (sync->last_render_end_us && now - sync->last_render_end_us > 500000)) {
        sync->window_begin_us = sync->window_first_start_us = now;
        sync->window_frames = sync->window_chunks = 0;
        sync->window_render_us = sync->window_te_us = sync->window_flush_us = sync->window_wait_us = 0;
        sync->window_outside_wait_us = sync->window_rest_us = sync->window_pixels = sync->window_max_render_us = 0;
    }
    sync->rendering = true;
    sync->render_begin_us = now;
    sync->frame_te_us = sync->frame_flush_us = sync->frame_wait_us = sync->frame_pixels = 0;
    sync->frame_chunks = sync->flush_depth = 0;
}

static void profile_finish(display_sync_t *sync, int64_t now)
{
    if (!sync->rendering) return;
    sync->rendering = false;
    sync->last_render_end_us = now;
    if (!sync->frame_chunks) return;
    const uint64_t render = elapsed_us(sync->render_begin_us, now);
    const uint64_t accounted = sync->frame_te_us + sync->frame_flush_us + sync->frame_wait_us;
    ++sync->window_frames;
    sync->window_chunks += sync->frame_chunks;
    sync->window_pixels += sync->frame_pixels;
    sync->window_render_us += render;
    sync->window_te_us += sync->frame_te_us;
    sync->window_flush_us += sync->frame_flush_us;
    sync->window_wait_us += sync->frame_wait_us;
    sync->window_rest_us += render > accounted ? render - accounted : 0;
    if (render > sync->window_max_render_us) sync->window_max_render_us = render;
    const uint64_t span = elapsed_us(sync->window_begin_us, now);
    if (span < 1500000) return;
    const uint64_t start_span = elapsed_us(sync->window_first_start_us, sync->render_begin_us);
    const uint32_t intervals = sync->window_frames - 1;
    const uint64_t period = intervals ? start_span / intervals : 0;
    const uint64_t rate = start_span ? (uint64_t)intervals * 100000000 / start_span : 0;
    /* flush excludes TE and any nested DMA wait. Residual is elapsed render
     * wall time minus those disjoint phases, not CPU time/utilization. */
    ESP_LOGI("display_perf", "active_us=%" PRIu64 " frames=%lu start_fps_x100=%" PRIu64
        " period_us=%" PRIu64 " chunks=%lu pixels=%" PRIu64 " render_us=%" PRIu64
        " te_us=%" PRIu64 " flushcb_us=%" PRIu64 " dmawait_us=%" PRIu64
        " rest_wall_us=%" PRIu64 " max_render_us=%" PRIu64 " outside_dma_us=%" PRIu64
        " render_avg_us=%" PRIu64 " rest_avg_us=%" PRIu64,
        span, (unsigned long)sync->window_frames, rate, period,
        (unsigned long)sync->window_chunks, sync->window_pixels, sync->window_render_us,
        sync->window_te_us, sync->window_flush_us, sync->window_wait_us,
        sync->window_rest_us, sync->window_max_render_us, sync->window_outside_wait_us,
        sync->window_render_us / sync->window_frames, sync->window_rest_us / sync->window_frames);
    sync->window_frames = 0;
}

void app_display_rounder(lv_area_t *area)
{
    if (!area) return;
    /* CO5300 datasheet pp159/161, restrictions on CASET(2Ah)/RASET(2Bh).
     * LVGL clips to the even-sized 480x480 panel before this callback. Its
     * get_max_row() also uses INVALIDATE_AREA, keeping partial chunks even. */
    area->x1 &= ~1;
    area->y1 &= ~1;
    area->x2 |= 1;
    area->y2 |= 1;
}

static void sync_event(lv_event_t *event)
{
    display_sync_t *sync = lv_event_get_user_data(event);
    switch (lv_event_get_code(event)) {
    case LV_EVENT_RENDER_START:
        profile_begin(sync, esp_timer_get_time());
        sync->first_flush = true;
        break;
    case LV_EVENT_FLUSH_START:
        if (sync->first_flush) {
            sync->first_flush = false;
            /* Wait after the first chunk is rendered, just before its QSPI
             * transfer. Following chunks in this refresh never wait again. */
            const int64_t te_begin = esp_timer_get_time();
            const esp_err_t error = co5300_display_wait_te(sync->io, 35);
            const int64_t now = esp_timer_get_time();
            if (sync->rendering) sync->frame_te_us += elapsed_us(te_begin, now);
            if (s_nes_display && s_nes_display == lv_event_get_target(event)) {
                const lv_area_t *area = lv_event_get_param(event);
                if (error == ESP_OK && area && area->x1 == s_nes_area.x1 &&
                    area->x2 == s_nes_area.x2 && area->y1 == s_nes_area.y1 &&
                    area->y2 <= s_nes_area.y2) {
                    arm_nes_te(s_nes_display, sync->io, te_begin, now);
                } else {
                    app_display_metrics_nes_arm(NULL, 0, 0, 0);
                }
            }
            if (error != ESP_OK) {
                portENTER_CRITICAL(&s_diagnostics_lock);
                if (sync->diagnostics_generation == s_diagnostics.generation &&
                    sync->io == s_diagnostics.io) {
                    s_diagnostics.error = error;
                    s_diagnostics.error_pending = true;
                }
                portEXIT_CRITICAL(&s_diagnostics_lock);
            }
        }
        if (sync->rendering && sync->flush_depth++ == 0) {
            sync->flush_begin_us = esp_timer_get_time();
            sync->flush_nested_wait_us = 0;
            ++sync->frame_chunks;
            const lv_area_t *area = lv_event_get_param(event);
            if (area && area->x2 >= area->x1 && area->y2 >= area->y1)
                sync->frame_pixels += (uint64_t)lv_area_get_width(area) * lv_area_get_height(area);
        }
        break;
    case LV_EVENT_FLUSH_FINISH:
        if (sync->rendering && sync->flush_depth && --sync->flush_depth == 0) {
            const uint64_t flush = elapsed_us(sync->flush_begin_us, esp_timer_get_time());
            sync->frame_flush_us += flush > sync->flush_nested_wait_us ? flush - sync->flush_nested_wait_us : 0;
        }
        break;
    case LV_EVENT_FLUSH_WAIT_START:
        if (sync->wait_depth++ == 0) {
            sync->wait_begin_us = esp_timer_get_time();
            sync->wait_in_render = sync->rendering;
        }
        break;
    case LV_EVENT_FLUSH_WAIT_FINISH:
        if (sync->wait_depth && --sync->wait_depth == 0) {
            const uint64_t wait = elapsed_us(sync->wait_begin_us, esp_timer_get_time());
            if (sync->wait_in_render && sync->rendering) {
                sync->frame_wait_us += wait;
                if (sync->flush_depth) sync->flush_nested_wait_us += wait;
            } else sync->window_outside_wait_us += wait;
        }
        break;
    case LV_EVENT_RENDER_READY:
        profile_finish(sync, esp_timer_get_time());
#ifdef AETHER_VIDEO_DIAGNOSTIC
        /* Observe after real rendering, since first FPU/PIE use can change
         * affinity. Both queries run before taking our short snapshot lock.
         * This mask observes sampled execution cores, not migration counts
         * or CPU utilisation; affinity=-1 is reported for tskNO_AFFINITY. */
        {
            const BaseType_t core = xPortGetCoreID();
            const BaseType_t affinity = xTaskGetCoreID(NULL);
            portENTER_CRITICAL(&s_diagnostics_lock);
            if (sync->diagnostics_generation == s_diagnostics.generation &&
                sync->io == s_diagnostics.io) {
                s_diagnostics.placement_sampled = true;
                s_diagnostics.last_core = core;
                s_diagnostics.affinity = affinity;
                if ((unsigned)core < 32U) s_diagnostics.core_mask |= UINT32_C(1) << core;
                if (s_diagnostics.placement_samples < UINT32_MAX) ++s_diagnostics.placement_samples;
            }
            portEXIT_CRITICAL(&s_diagnostics_lock);
        }
#endif
        break;
    case LV_EVENT_DELETE:
        if (s_nes_display && s_nes_display == lv_event_get_target(event))
            app_display_sync_set_nes_viewport(s_nes_display, NULL);
        portENTER_CRITICAL(&s_diagnostics_lock);
        if (sync->diagnostics_generation == s_diagnostics.generation &&
            sync->io == s_diagnostics.io) {
            s_diagnostics.io = NULL;
            s_diagnostics.error_pending = false;
#ifdef AETHER_VIDEO_DIAGNOSTIC
            s_diagnostics.placement_sampled = false;
#endif
        }
        portEXIT_CRITICAL(&s_diagnostics_lock);
        free(sync);
        break;
    default:
        break;
    }
}

esp_err_t app_display_sync_attach(lv_display_t *display, esp_lcd_panel_io_handle_t io)
{
    if (!display || !io) return ESP_ERR_INVALID_ARG;
    display_sync_t *sync = calloc(1, sizeof(*sync));
    if (!sync) return ESP_ERR_NO_MEM;
    sync->io = io;
    portENTER_CRITICAL(&s_diagnostics_lock);
    sync->diagnostics_generation = ++s_diagnostics.generation;
    s_diagnostics.io = io;
    s_diagnostics.error_pending = false;
    s_diagnostics.error_after_us = s_diagnostics.frequency_after_us = 0;
#ifdef AETHER_VIDEO_DIAGNOSTIC
    s_diagnostics.placement_sampled = false;
    s_diagnostics.core_mask = s_diagnostics.placement_samples = 0;
#endif
    portEXIT_CRITICAL(&s_diagnostics_lock);
    lv_display_add_event_cb(display, sync_event, LV_EVENT_ALL, sync);
    return ESP_OK;
}
