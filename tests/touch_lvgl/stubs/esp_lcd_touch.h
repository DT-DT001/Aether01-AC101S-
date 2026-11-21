#pragma once
#include <stdint.h>
#include "esp_err.h"
#define CONFIG_ESP_LCD_TOUCH_MAX_POINTS 2
#define GPIO_NUM_NC -1
typedef struct { uint16_t x, y; } esp_lcd_touch_point_data_t;
typedef struct esp_lcd_touch {
    struct { int int_gpio_num; void *user_data; } config;
} *esp_lcd_touch_handle_t;
esp_err_t esp_lcd_touch_read_data(esp_lcd_touch_handle_t);
esp_err_t esp_lcd_touch_get_data(esp_lcd_touch_handle_t,esp_lcd_touch_point_data_t *,uint8_t *,uint8_t);
esp_err_t esp_lcd_touch_register_interrupt_callback_with_data(esp_lcd_touch_handle_t,void (*)(esp_lcd_touch_handle_t),void *);
