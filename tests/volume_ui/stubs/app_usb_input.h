#pragma once
#include "lvgl.h"
#include "esp_err.h"
static inline esp_err_t app_usb_input_modal_open(lv_obj_t *modal, lv_obj_t *preferred)
{ (void)modal; (void)preferred; return ESP_OK; }
static inline void app_usb_input_modal_close(lv_obj_t *modal) { (void)modal; }
