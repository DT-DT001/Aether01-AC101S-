#pragma once
#include "esp_err.h"
static inline esp_err_t aether_usb_camera_class_start(void *context)
{ (void)context; return ESP_OK; }
static inline void aether_usb_camera_class_request_stop(void *context)
{ (void)context; }
static inline esp_err_t aether_usb_camera_class_poll_stopped(void *context)
{ (void)context; return ESP_OK; }
