#pragma once
#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"
#include "lvgl.h"
bool app_usb_input_capture_keys(bool enable);
uint8_t app_usb_input_take_captured_usage(void);
esp_err_t app_usb_input_modal_open(lv_obj_t *modal, lv_obj_t *preferred);
