#pragma once
#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "aether_storage_probe.h"
typedef struct { void *pmic; } aether_board_t;
typedef struct aether_storage_dev_t *aether_storage_handle_t;
typedef enum { AETHER_STORAGE_NO_CARD, AETHER_STORAGE_MOUNTING, AETHER_STORAGE_READY,
               AETHER_STORAGE_ERROR, AETHER_STORAGE_REMOVING, AETHER_STORAGE_USB_PREPARING, AETHER_STORAGE_USB_EXPORTED } aether_storage_state_t;
typedef struct { aether_storage_state_t state; bool card_inserted, mounted, usb_exported;
                 uint32_t usb_clock_khz; bool usb_high_speed_fallback;
                 uint64_t capacity_bytes; esp_err_t last_error; } aether_storage_status_t;
#define AETHER_STORAGE_MOUNT_POINT "/sdcard"
esp_err_t aether_board_set_sd_power(aether_board_t *, bool);
