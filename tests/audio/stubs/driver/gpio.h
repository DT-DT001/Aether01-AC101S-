#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef int gpio_num_t;
typedef struct { uint64_t pin_bit_mask; int mode, pull_up_en, pull_down_en, intr_type; } gpio_config_t;
#define GPIO_MODE_OUTPUT 1
#define GPIO_PULLUP_DISABLE 0
#define GPIO_PULLDOWN_ENABLE 1
#define GPIO_INTR_DISABLE 0
#define GPIO_IS_VALID_OUTPUT_GPIO(pin) ((pin) >= 0 && (pin) < 60)
esp_err_t gpio_config(const gpio_config_t *);
esp_err_t gpio_set_level(gpio_num_t, uint32_t);
esp_err_t gpio_reset_pin(gpio_num_t);
esp_err_t gpio_input_enable(gpio_num_t);
esp_err_t gpio_set_direction(gpio_num_t, int);
int gpio_get_level(gpio_num_t);
