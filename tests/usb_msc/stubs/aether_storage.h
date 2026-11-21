#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
typedef void *aether_storage_handle_t;
typedef struct { bool card_inserted,mounted,usb_exported; uint64_t capacity_bytes; uint32_t usb_clock_khz; bool usb_high_speed_fallback; esp_err_t last_error; } aether_storage_status_t;
esp_err_t aether_storage_get_status(aether_storage_handle_t,aether_storage_status_t*);
esp_err_t aether_storage_usb_begin(aether_storage_handle_t,uint32_t);
esp_err_t aether_storage_usb_end(aether_storage_handle_t,uint32_t);
bool aether_storage_usb_owned(aether_storage_handle_t);
esp_err_t aether_storage_usb_read(aether_storage_handle_t,uint64_t,void*,size_t);
esp_err_t aether_storage_usb_write(aether_storage_handle_t,uint64_t,const void*,size_t);
esp_err_t aether_storage_usb_sync(aether_storage_handle_t);
