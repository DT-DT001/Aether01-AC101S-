#pragma once
typedef int gpio_num_t;
#include "esp_err.h"
esp_err_t gpio_set_level(int pin, int value);
