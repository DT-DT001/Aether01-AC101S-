#include "app_touch.h"

#include <stdatomic.h>
#include "cst9220.h"
#include "esp_log.h"
#include "esp_lvgl_port.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#define TOUCH_STALE_US 1800000LL
#define TOUCH_UNMATCHED_IRQ_US 64000LL

typedef struct {
    esp_lcd_touch_handle_t touch;
    app_power_t *power;
    lv_indev_t *indev;
    int64_t retry_after_us;
    int64_t log_after_us;
    atomic_bool consume_gesture;
    atomic_bool enabled;
    atomic_uint irq_count;
    bool pressed, have_report, transport_blocked, stale_report, unmatched_irq;
    uint32_t report_sequence, report_irq;
    int64_t report_us, unmatched_irq_us;
    int64_t perf_begin_us;
    uint32_t perf_reads, perf_pressed, perf_moves, perf_errors, perf_irq_base;
    uint64_t perf_i2c_us;
    uint16_t perf_x, perf_y;
    bool perf_down;
} touch_context_t;

/* One fitted touch controller, retained for the entire application lifetime. */
static touch_context_t s_touch;
static atomic_bool s_touch_registered;

static void cancel_press(touch_context_t *ctx)
{
    if (!ctx->pressed) return;
    ctx->pressed = false;
    /* LVGL reset clears the widget's pressed/scroll state and notifies its
     * handlers. Reporting an ordinary release here could activate the widget. */
    lv_indev_reset(ctx->indev, NULL);
}

/* Observation only: reads and coordinate changes are software samples, not
 * the controller's physical scan rate. No per-report log or task allocation. */
static void touch_profile(touch_context_t *ctx, int64_t began, int64_t finished,
                          esp_err_t error, uint8_t count,
                          const esp_lcd_touch_point_data_t *points)
{
    if (!ctx->perf_begin_us) ctx->perf_begin_us = began;
    ++ctx->perf_reads;
    if (finished > began) ctx->perf_i2c_us += (uint64_t)(finished - began);
    if (error != ESP_OK) ++ctx->perf_errors;
    else {
        if (count) {
            ++ctx->perf_pressed;
            if (!ctx->perf_down || ctx->perf_x != points[0].x || ctx->perf_y != points[0].y) ++ctx->perf_moves;
            ctx->perf_x = points[0].x; ctx->perf_y = points[0].y;
        }
        ctx->perf_down = count != 0;
    }
    if (began - ctx->perf_begin_us < 1500000) return;
    if (ctx->perf_pressed) {
        ESP_LOGI("touch_perf", "window_ms=%lu reads=%lu pressed=%lu changed=%lu irq=%lu errors=%lu i2c_avg_us=%lu",
            (unsigned long)((began - ctx->perf_begin_us) / 1000), (unsigned long)ctx->perf_reads,
            (unsigned long)ctx->perf_pressed, (unsigned long)ctx->perf_moves,
            (unsigned long)(atomic_load(&ctx->irq_count) - ctx->perf_irq_base),
            (unsigned long)ctx->perf_errors, (unsigned long)(ctx->perf_i2c_us / ctx->perf_reads));
    }
    ctx->perf_begin_us = began;
    ctx->perf_reads = ctx->perf_pressed = ctx->perf_moves = ctx->perf_errors = 0;
    ctx->perf_i2c_us = 0;
    ctx->perf_irq_base = atomic_load(&ctx->irq_count);
}

static void touch_read(lv_indev_t *indev, lv_indev_data_t *data)
{
    touch_context_t *ctx = lv_indev_get_driver_data(indev);
    const int64_t now = esp_timer_get_time();
    data->state = LV_INDEV_STATE_RELEASED;
    if (now < ctx->retry_after_us) return;

    const uint32_t irq = atomic_load(&ctx->irq_count);
    uint8_t count = 0;
    esp_lcd_touch_point_data_t points[CONFIG_ESP_LCD_TOUCH_MAX_POINTS] = {0};
    esp_err_t err = esp_lcd_touch_read_data(ctx->touch);
    if (err == ESP_OK) err = esp_lcd_touch_get_data(ctx->touch, points, &count,
                                                   CONFIG_ESP_LCD_TOUCH_MAX_POINTS);
    const int64_t sampled_at = esp_timer_get_time();
    const uint32_t sequence = cst9220_report_sequence(ctx->touch);
    if (sequence != ctx->report_sequence) {
        ctx->have_report = true;
        ctx->report_sequence = sequence;
        ctx->report_irq = irq;
        ctx->report_us = sampled_at;
        ctx->unmatched_irq = false;
        if (ctx->stale_report) ctx->transport_blocked = false;
        ctx->stale_report = false;
    }
    touch_profile(ctx, now, sampled_at, err, count, points);
    if (err != ESP_OK) {
        /* Never abort the MCU for a recoverable peripheral timeout, nor keep
         * the previous pressed coordinate alive. Back off on a broken bus. */
        ctx->retry_after_us = now + 250000;
        /* Cancel this whole physical gesture. If the controller recovers
         * while the finger is still held, it must not become a new click. */
        ctx->transport_blocked = true;
        cancel_press(ctx);
        if (now >= ctx->log_after_us) {
            ESP_LOGW("app_touch", "read failed: %s", esp_err_to_name(err));
            ctx->log_after_us = now + 2000000;
        }
        return;
    }
    ctx->retry_after_us = 0;
    const bool sleeping = ctx->power && app_power_is_sleeping(ctx->power);
    if (sleeping || atomic_load(&ctx->consume_gesture)) cancel_press(ctx);
    if (count == 0) {
        ctx->pressed = false;
        ctx->transport_blocked = false;
        ctx->stale_report = false;
        ctx->unmatched_irq = false;
        atomic_store(&ctx->consume_gesture, false);
        return;
    }
    if (points[0].x >= lv_display_get_horizontal_resolution(lv_indev_get_display(indev)) ||
        points[0].y >= lv_display_get_vertical_resolution(lv_indev_get_display(indev))) {
        ctx->transport_blocked = true;
        cancel_press(ctx);
        return;
    }
    if (sleeping) {
        /* Screen-off touch is consumed, including a finger held across a
         * later power-key wake. Only an explicit wake source lights the panel. */
        atomic_store(&ctx->consume_gesture, true);
        return;
    }
    /* A held screen-off/key-wake gesture must not click the foreground page. */
    if (atomic_load(&ctx->consume_gesture)) return;
    if (ctx->stale_report) return;
    if (ctx->have_report) {
        if (irq != ctx->report_irq && !ctx->unmatched_irq) {
            ctx->unmatched_irq = true;
            ctx->unmatched_irq_us = sampled_at;
        }
        /* Polls faster than the controller can return no new data while a
         * real finger is held. Give it time to report, with a shorter
         * recovery window when a new IRQ did not yield any valid snapshot. */
        if (sampled_at - ctx->report_us >= TOUCH_STALE_US ||
            (ctx->unmatched_irq && sampled_at - ctx->unmatched_irq_us >= TOUCH_UNMATCHED_IRQ_US)) {
            ctx->stale_report = true;
            cancel_press(ctx);
            ESP_LOGW("app_touch", "discarded stale contact age_ms=%lu unmatched_irq=%u",
                     (unsigned long)((sampled_at - ctx->report_us) / 1000), (unsigned)ctx->unmatched_irq);
            return;
        }
    }
    if (ctx->transport_blocked) return;
    data->point.x = points[0].x;
    data->point.y = points[0].y;
    data->state = LV_INDEV_STATE_PRESSED;
    ctx->pressed = true;
}

static void touch_interrupt(esp_lcd_touch_handle_t touch)
{
    touch_context_t *ctx = touch->config.user_data;
    if (!ctx || !ctx->indev) return;
    atomic_fetch_add(&ctx->irq_count, 1);
    /* This installed esp_lvgl_port implementation handles ISR context itself. */
    lvgl_port_task_wake(LVGL_PORT_EVENT_TOUCH, ctx->indev);
}

void app_touch_set_enabled(lv_indev_t *indev, bool enabled)
{
    if (!indev) return;
    if (indev == s_touch.indev) {
        /* Publish exclusivity before LVGL stops polling. When raw input gives
         * ownership back, consume held fingers until a real release so the
         * NES exit gesture cannot also activate a button on the next page. */
        atomic_store(&s_touch.enabled, enabled);
        if (!enabled) cancel_press(&s_touch);
        if (enabled) atomic_store(&s_touch.consume_gesture, true);
    }
    lv_indev_enable(indev, enabled);
}

void app_touch_consume_wake_gesture(void)
{
    if (atomic_load(&s_touch_registered) && atomic_load(&s_touch.enabled))
        atomic_store(&s_touch.consume_gesture, true);
}

lv_indev_t *app_touch_add(lv_display_t *display, esp_lcd_touch_handle_t touch,
                          app_power_t *power)
{
    if (!display || !touch || s_touch.indev || !lvgl_port_lock(1000)) return NULL;
    lv_indev_t *indev = lv_indev_create();
    if (indev) {
        s_touch = (touch_context_t){.touch = touch, .power = power, .indev = indev};
        atomic_init(&s_touch.consume_gesture, false);
        atomic_init(&s_touch.enabled, true);
        atomic_init(&s_touch.irq_count, 0);
        lv_indev_set_type(indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_disp(indev, display);
        lv_indev_set_driver_data(indev, &s_touch);
        lv_indev_set_read_cb(indev, touch_read);
        /* Keep timer polling as well as IRQ wake, so a lost release IRQ or
         * a transient I2C error can recover without another physical touch. */
        lv_timer_set_period(lv_indev_get_read_timer(indev), 8);
        if (touch->config.int_gpio_num != GPIO_NUM_NC) {
            esp_err_t err = esp_lcd_touch_register_interrupt_callback_with_data(touch, touch_interrupt, &s_touch);
            if (err != ESP_OK) ESP_LOGW("app_touch", "IRQ unavailable, using polling: %s", esp_err_to_name(err));
        }
        atomic_store(&s_touch_registered, true);
        ESP_LOGI("app_touch", "touch polling=8 ms; screen-off touch wake disabled");
    }
    lvgl_port_unlock();
    return indev;
}
