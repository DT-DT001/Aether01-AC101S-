#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <stdatomic.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_STATE 0x103
#define ESP_ERR_NOT_SUPPORTED 0x106
typedef pthread_mutex_t portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED PTHREAD_MUTEX_INITIALIZER
#define portENTER_CRITICAL(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL(p) pthread_mutex_unlock(p)
#define portENTER_CRITICAL_ISR(p) pthread_mutex_lock(p)
#define portEXIT_CRITICAL_ISR(p) pthread_mutex_unlock(p)
typedef uint32_t TickType_t;
typedef int BaseType_t;
#define pdTRUE 1
#define pdFALSE 0
#define pdMS_TO_TICKS(ms) (ms)
#define portYIELD_FROM_ISR() test_yield_from_isr()
void test_yield_from_isr(void);
typedef struct test_display lv_display_t;
typedef struct { int32_t x1,y1,x2,y2; } lv_area_t;
typedef void (*lv_display_flush_cb_t)(lv_display_t *,const lv_area_t *,uint8_t *);
typedef void (*lv_display_flush_wait_cb_t)(lv_display_t *);
typedef enum { LV_DISPLAY_RENDER_MODE_PARTIAL,LV_DISPLAY_RENDER_MODE_FULL } lv_display_render_mode_t;
typedef enum { LV_COLOR_FORMAT_RGB565=1,LV_COLOR_FORMAT_RGB888 } lv_color_format_t;
typedef struct {
    struct { uint32_t w,h,stride,cf; } header;
    uint32_t data_size;
    uint8_t *data;
} lv_draw_buf_t;
struct test_display {
    atomic_bool last,flushing;
    lv_display_flush_cb_t flush_cb;
    lv_display_flush_wait_cb_t flush_wait_cb;
    lv_display_render_mode_t render_mode;
    lv_color_format_t color_format;
    int32_t hres,vres;
    lv_draw_buf_t *active;
};
static inline lv_display_render_mode_t lv_display_get_render_mode(lv_display_t *d) { return d->render_mode; }
static inline lv_color_format_t lv_display_get_color_format(lv_display_t *d) { return d->color_format; }
static inline int32_t lv_display_get_horizontal_resolution(const lv_display_t *d) { return d->hres; }
static inline int32_t lv_display_get_vertical_resolution(const lv_display_t *d) { return d->vres; }
static inline lv_draw_buf_t *lv_display_get_buf_active(lv_display_t *d) { return d->active; }
void lv_display_set_flush_cb(lv_display_t *,lv_display_flush_cb_t);
void lv_display_set_flush_wait_cb(lv_display_t *,lv_display_flush_wait_cb_t);
typedef void *esp_lcd_panel_io_handle_t;
typedef struct { int unused; } esp_lcd_panel_io_event_data_t;
typedef struct { bool (*on_color_trans_done)(esp_lcd_panel_io_handle_t, esp_lcd_panel_io_event_data_t *, void *); } esp_lcd_panel_io_callbacks_t;
esp_err_t esp_lcd_panel_io_register_event_callbacks(esp_lcd_panel_io_handle_t, const esp_lcd_panel_io_callbacks_t *, void *);
bool lv_display_flush_is_last(lv_display_t *);
void lvgl_port_flush_ready(lv_display_t *);
int64_t esp_timer_get_time(void);
