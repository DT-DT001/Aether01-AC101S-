#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
enum {GPIO_NUM_37=37, GPIO_NUM_38, GPIO_NUM_39, GPIO_NUM_40};
enum {GPIO_MODE_DISABLE, GPIO_MODE_INPUT, GPIO_MODE_OUTPUT_OD};
enum {GPIO_PULLUP_DISABLE, GPIO_PULLUP_ENABLE};
enum {GPIO_PULLDOWN_DISABLE, GPIO_PULLDOWN_ENABLE};
enum {GPIO_INTR_DISABLE, GPIO_INTR_POSEDGE};
enum {GPIO_PULLUP_ONLY, GPIO_FLOATING};
typedef struct {uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type;} gpio_config_t;
esp_err_t gpio_config(const gpio_config_t *config);
esp_err_t gpio_set_level(gpio_num_t pin, uint32_t level);
esp_err_t gpio_set_pull_mode(gpio_num_t pin, int mode);
esp_err_t gpio_set_intr_type(gpio_num_t pin, int type);
esp_err_t gpio_install_isr_service(int flags);
esp_err_t gpio_isr_handler_add(gpio_num_t pin, void (*handler)(void *), void *context);
esp_err_t gpio_isr_handler_remove(gpio_num_t pin);
esp_err_t gpio_intr_disable(gpio_num_t pin);
esp_err_t gpio_intr_enable(gpio_num_t pin);
esp_err_t gpio_reset_pin(gpio_num_t pin);
