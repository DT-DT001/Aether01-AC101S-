#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
typedef int esp_err_t;
#define ESP_OK 0
#define ESP_FAIL -1
#define ESP_ERR_NO_MEM 0x101
#define ESP_ERR_INVALID_ARG 0x102
#define ESP_ERR_INVALID_RESPONSE 0x108
#define ESP_ERR_TIMEOUT 0x107
typedef uint32_t TickType_t;
typedef int portMUX_TYPE;
#define portMUX_INITIALIZER_UNLOCKED 0
#define portENTER_CRITICAL(p) ((void)(p))
#define portEXIT_CRITICAL(p) ((void)(p))
#define pdMS_TO_TICKS(ms) (ms)
void vTaskDelay(TickType_t);
#define ESP_RETURN_ON_ERROR(expr, tag, ...) do { (void)(tag); esp_err_t test_err = (expr); if (test_err != ESP_OK) return test_err; } while (0)
#define ESP_RETURN_ON_FALSE(expr, err, tag, ...) do { (void)(tag); if (!(expr)) return (err); } while (0)
#define ESP_GOTO_ON_ERROR(expr, label, tag, ...) do { (void)(tag); ret = (expr); if (ret != ESP_OK) goto label; } while (0)
#define ESP_GOTO_ON_FALSE(expr, err, label, tag, ...) do { (void)(tag); if (!(expr)) { ret = (err); goto label; } } while (0)
#define ESP_LOGI(tag, ...) ((void)(tag))
typedef void *i2c_master_bus_handle_t;
typedef void *i2c_master_dev_handle_t;
typedef struct { int dev_addr_length, device_address, scl_speed_hz; } i2c_device_config_t;
#define I2C_ADDR_BIT_LEN_7 7
esp_err_t i2c_master_bus_add_device(i2c_master_bus_handle_t, const i2c_device_config_t *, i2c_master_dev_handle_t *);
esp_err_t i2c_master_bus_rm_device(i2c_master_dev_handle_t);
esp_err_t i2c_master_transmit(i2c_master_dev_handle_t, const uint8_t *, size_t, int);
esp_err_t i2c_master_transmit_receive(i2c_master_dev_handle_t, const uint8_t *, size_t, uint8_t *, size_t, int);
#define GPIO_NUM_NC (-1)
#define GPIO_MODE_INPUT 1
#define GPIO_MODE_OUTPUT 2
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_DISABLE 0
#define GPIO_INTR_DISABLE 0
#define GPIO_INTR_POSEDGE 1
#define GPIO_INTR_NEGEDGE 2
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(int, int);
esp_err_t gpio_reset_pin(int);
typedef struct esp_lcd_touch_s esp_lcd_touch_t;
typedef esp_lcd_touch_t *esp_lcd_touch_handle_t;
typedef struct {
    uint16_t x_max, y_max;
    int rst_gpio_num, int_gpio_num;
    struct { int reset, interrupt; } levels;
} esp_lcd_touch_config_t;
typedef struct { uint16_t x, y, strength; uint8_t track_id; } esp_lcd_touch_point_data_t;
struct esp_lcd_touch_s {
    esp_lcd_touch_config_t config;
    struct { portMUX_TYPE lock; uint8_t points; esp_lcd_touch_point_data_t coords[2]; } data;
    esp_err_t (*read_data)(esp_lcd_touch_handle_t);
    bool (*get_xy)(esp_lcd_touch_handle_t, uint16_t *, uint16_t *, uint16_t *, uint8_t *, uint8_t);
    esp_err_t (*get_track_id)(esp_lcd_touch_handle_t, uint8_t *, uint8_t);
    esp_err_t (*enter_sleep)(esp_lcd_touch_handle_t);
    esp_err_t (*exit_sleep)(esp_lcd_touch_handle_t);
    esp_err_t (*del)(esp_lcd_touch_handle_t);
};
