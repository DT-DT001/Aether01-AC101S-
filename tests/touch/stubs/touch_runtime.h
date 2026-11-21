#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_TIMEOUT 0x107
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 2
#define GPIO_NUM_NC -1
#define ESP_LOGW(tag, ...) ((void)(tag))
#define LVGL_PORT_EVENT_TOUCH 2
#define LV_INDEV_STATE_RELEASED 0
#define LV_INDEV_STATE_PRESSED 1
#define LV_INDEV_TYPE_POINTER 1
#define pdFALSE 0
#define pdTRUE 1
#define pdPASS 1
#define portMAX_DELAY UINT32_MAX
typedef int BaseType_t;
typedef uint32_t TickType_t;
typedef void *TaskHandle_t;
BaseType_t xTaskCreate(void (*)(void *), const char *, uint32_t, void *, unsigned, TaskHandle_t *);
uint32_t ulTaskNotifyTake(BaseType_t, TickType_t);
void vTaskNotifyGiveFromISR(TaskHandle_t, BaseType_t *);
void test_yield_from_isr(void);
#define portYIELD_FROM_ISR() test_yield_from_isr()
typedef struct { bool display_sleeping; } aether_board_t;
typedef struct { int width; int height; } lv_display_t;
typedef struct { unsigned period; } lv_timer_t;
typedef struct { int state; struct { int x; int y; } point; } lv_indev_data_t;
typedef struct lv_indev lv_indev_t;
struct lv_indev {
    void *driver_data;
    lv_display_t *display;
    lv_timer_t timer;
    void (*read_cb)(lv_indev_t *, lv_indev_data_t *);
    int type;
    bool enabled;
};
typedef struct { uint16_t x; uint16_t y; } esp_lcd_touch_point_data_t;
typedef struct esp_lcd_touch {
    struct { int int_gpio_num; void *user_data; } config;
} *esp_lcd_touch_handle_t;
int64_t esp_timer_get_time(void);
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t, esp_lcd_touch_point_data_t *, uint8_t *, uint8_t);
esp_err_t esp_lcd_touch_register_interrupt_callback_with_data(esp_lcd_touch_handle_t,
                            void (*)(esp_lcd_touch_handle_t), void *);
void *lv_indev_get_driver_data(lv_indev_t *);
lv_display_t *lv_indev_get_display(lv_indev_t *);
int lv_display_get_horizontal_resolution(lv_display_t *);
int lv_display_get_vertical_resolution(lv_display_t *);
bool lvgl_port_lock(uint32_t);
void lvgl_port_unlock(void);
void lvgl_port_task_wake(int, void *);
lv_indev_t *lv_indev_create(void);
void lv_indev_set_type(lv_indev_t *, int);
void lv_indev_enable(lv_indev_t *, bool);
void lv_indev_set_disp(lv_indev_t *, lv_display_t *);
void lv_indev_set_driver_data(lv_indev_t *, void *);
void lv_indev_set_read_cb(lv_indev_t *, void (*)(lv_indev_t *, lv_indev_data_t *));
void lv_indev_reset(lv_indev_t *, void *);
lv_timer_t *lv_indev_get_read_timer(lv_indev_t *);
void lv_timer_set_period(lv_timer_t *, uint32_t);
