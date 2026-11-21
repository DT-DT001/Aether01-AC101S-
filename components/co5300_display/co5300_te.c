#include "co5300_te.h"
#include <string.h>
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"

/* CO5300 datasheet 5.3 / TEON(35h): M=0, N=0 is high during V-blank.
 * The GPIO ISR only publishes a rising edge; rendering waits in task context. */
static portMUX_TYPE s_lock = portMUX_INITIALIZER_UNLOCKED;
static struct {
    esp_lcd_panel_io_handle_t io;
    gpio_num_t pin;
    SemaphoreHandle_t signal;
    bool handler_added, waiting, closing;
    unsigned publishers;
    uint32_t cancel_sequence, timeout_edges;
    int64_t last_edge_us, retry_after_us;
    co5300_te_metrics_t metrics;
} s_te;

static void te_rising(void *argument)
{
    (void)argument;
    const int64_t now = esp_timer_get_time();
    BaseType_t wake = pdFALSE;
    portENTER_CRITICAL_ISR(&s_lock);
    SemaphoreHandle_t signal = !s_te.closing ? s_te.signal : NULL;
    if (!signal) { portEXIT_CRITICAL_ISR(&s_lock); return; }
    ++s_te.publishers;
    if (s_te.last_edge_us && now > s_te.last_edge_us) {
        const int64_t period = now - s_te.last_edge_us;
        s_te.metrics.last_period_us = period > UINT32_MAX ? UINT32_MAX : (uint32_t)period;
    }
    s_te.last_edge_us = now;
    ++s_te.metrics.edges;
    portEXIT_CRITICAL_ISR(&s_lock);
    xSemaphoreGiveFromISR(signal, &wake);
    portENTER_CRITICAL_ISR(&s_lock);
    --s_te.publishers;
    portEXIT_CRITICAL_ISR(&s_lock);
    if (wake) portYIELD_FROM_ISR();
}

esp_err_t co5300_te_init(esp_lcd_panel_io_handle_t io, gpio_num_t pin)
{
    if (!io) return ESP_ERR_INVALID_ARG;
    if (s_te.io) return ESP_ERR_INVALID_STATE;
    s_te.io = io;
    s_te.pin = pin;
    s_te.metrics.pin = pin;
    if (pin == GPIO_NUM_NC) { s_te.metrics.setup_error = ESP_ERR_NOT_SUPPORTED; return ESP_ERR_NOT_SUPPORTED; }
    if ((unsigned)pin >= 64U) { s_te.metrics.setup_error = ESP_ERR_INVALID_ARG; return ESP_ERR_INVALID_ARG; }
    esp_err_t error = ESP_OK;
    s_te.signal = xSemaphoreCreateBinary();
    if (!s_te.signal) { error = ESP_ERR_NO_MEM; goto fail; }
    const gpio_config_t config = {
        .pin_bit_mask = 1ULL << pin, .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_DISABLE, .pull_down_en = GPIO_PULLDOWN_DISABLE,
        .intr_type = GPIO_INTR_POSEDGE,
    };
    error = gpio_config(&config);
    if (error != ESP_OK) goto fail;
    error = gpio_install_isr_service(0);
    if (error != ESP_OK && error != ESP_ERR_INVALID_STATE) goto fail;
    error = gpio_isr_handler_add(pin, te_rising, NULL);
    if (error != ESP_OK) goto fail;
    s_te.handler_added = true;
    s_te.metrics.available = true;
    return ESP_OK;
fail:
    gpio_intr_disable(pin);
    if (s_te.signal) { vSemaphoreDelete(s_te.signal); s_te.signal = NULL; }
    s_te.metrics.setup_error = error;
    return error;
}

void co5300_display_cancel_te(esp_lcd_panel_io_handle_t io)
{
    portENTER_CRITICAL(&s_lock);
    const bool matches = io && io == s_te.io && !s_te.closing;
    if (matches) {
        ++s_te.cancel_sequence;
        s_te.retry_after_us = 0;
        s_te.last_edge_us = 0;
        s_te.metrics.last_period_us = 0; /* Do not report the sleep gap as a panel period. */
    }
    SemaphoreHandle_t signal = matches ? s_te.signal : NULL;
    if (signal) ++s_te.publishers;
    portEXIT_CRITICAL(&s_lock);
    if (signal) {
        xSemaphoreGive(signal);
        portENTER_CRITICAL(&s_lock);
        --s_te.publishers;
        portEXIT_CRITICAL(&s_lock);
    }
}

esp_err_t co5300_te_deinit(esp_lcd_panel_io_handle_t io)
{
    if (!io || io != s_te.io) return ESP_ERR_INVALID_ARG;
    co5300_display_cancel_te(io);
    portENTER_CRITICAL(&s_lock);
    const bool waiting = s_te.waiting;
    if (!waiting) { s_te.closing = true; s_te.metrics.available = false; }
    portEXIT_CRITICAL(&s_lock);
    if (waiting) return ESP_ERR_INVALID_STATE;
    if (s_te.handler_added) {
        gpio_intr_disable(s_te.pin);
        gpio_isr_handler_remove(s_te.pin);
        s_te.handler_added = false;
    }
    /* A GPIO ISR may have been dispatched on the other CPU just before
     * removal. Publishers own the semaphore until Give has returned. Never
     * call a FreeRTOS API under our context spinlock. */
    const int64_t deadline = esp_timer_get_time() + 100000;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const unsigned publishers = s_te.publishers;
        portEXIT_CRITICAL(&s_lock);
        if (!publishers) break;
        if (esp_timer_get_time() >= deadline) return ESP_ERR_TIMEOUT;
        vTaskDelay(1);
    }
    portENTER_CRITICAL(&s_lock);
    SemaphoreHandle_t signal = s_te.signal;
    /* An ISR dispatched just before removal sees NULL after this lock. */
    memset(&s_te, 0, sizeof(s_te));
    portEXIT_CRITICAL(&s_lock);
    if (signal) vSemaphoreDelete(signal);
    return ESP_OK;
}

esp_err_t co5300_display_wait_te(esp_lcd_panel_io_handle_t io, uint32_t timeout_ms)
{
    if (!io || timeout_ms > 100) return ESP_ERR_INVALID_ARG;
    const int64_t started = esp_timer_get_time();
    portENTER_CRITICAL(&s_lock);
    if (io != s_te.io || !s_te.metrics.available || s_te.closing) {
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_te.waiting) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    if (started < s_te.retry_after_us && s_te.metrics.edges == s_te.timeout_edges) {
        ++s_te.metrics.fallback_frames;
        portEXIT_CRITICAL(&s_lock);
        return ESP_ERR_TIMEOUT;
    }
    s_te.waiting = true;
    ++s_te.metrics.waits;
    const uint32_t edge = s_te.metrics.edges, cancellation = s_te.cancel_sequence;
    portEXIT_CRITICAL(&s_lock);
    xSemaphoreTake(s_te.signal, 0); /* A binary semaphore contains at most one stale token. */
    const int64_t deadline = started + (int64_t)timeout_ms * 1000;
    esp_err_t result;
    for (;;) {
        portENTER_CRITICAL(&s_lock);
        const bool cancelled = cancellation != s_te.cancel_sequence;
        const bool fresh = edge != s_te.metrics.edges;
        portEXIT_CRITICAL(&s_lock);
        if (cancelled) { result = ESP_ERR_INVALID_STATE; break; }
        if (fresh) { result = ESP_OK; break; }
        const int64_t remaining = deadline - esp_timer_get_time();
        if (remaining <= 0) { result = ESP_ERR_TIMEOUT; break; }
        TickType_t ticks = pdMS_TO_TICKS((uint32_t)((remaining + 999) / 1000));
        xSemaphoreTake(s_te.signal, ticks ? ticks : 1);
    }
    const int64_t elapsed = esp_timer_get_time() - started;
    portENTER_CRITICAL(&s_lock);
    s_te.metrics.last_wait_us = elapsed > UINT32_MAX ? UINT32_MAX : elapsed < 0 ? 0 : (uint32_t)elapsed;
    if (s_te.metrics.last_wait_us > s_te.metrics.max_wait_us) s_te.metrics.max_wait_us = s_te.metrics.last_wait_us;
    if (result == ESP_OK) { ++s_te.metrics.synced; s_te.retry_after_us = 0; }
    else if (result == ESP_ERR_TIMEOUT) {
        ++s_te.metrics.timeouts; s_te.timeout_edges = s_te.metrics.edges;
        s_te.retry_after_us = esp_timer_get_time() + 250000;
    } else ++s_te.metrics.cancelled;
    s_te.waiting = false;
    portEXIT_CRITICAL(&s_lock);
    return result;
}

esp_err_t co5300_display_get_te_metrics(esp_lcd_panel_io_handle_t io, co5300_te_metrics_t *metrics)
{
    if (!io || !metrics) return ESP_ERR_INVALID_ARG;
    portENTER_CRITICAL(&s_lock);
    if (io != s_te.io) { portEXIT_CRITICAL(&s_lock); return ESP_ERR_INVALID_STATE; }
    *metrics = s_te.metrics;
    portEXIT_CRITICAL(&s_lock);
    return ESP_OK;
}
