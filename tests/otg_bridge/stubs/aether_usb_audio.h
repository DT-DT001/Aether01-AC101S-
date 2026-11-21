#pragma once
#include "aether_usb_host_class.h"
static inline esp_err_t aether_usb_audio_class_start(void *c){(void)c;return ESP_OK;}
static inline void aether_usb_audio_class_request_stop(void *c){(void)c;}
static inline esp_err_t aether_usb_audio_class_poll_stopped(void *c){(void)c;return ESP_OK;}
