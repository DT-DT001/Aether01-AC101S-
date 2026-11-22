#pragma once
#include "esp_err.h"
#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool active;
    uint8_t keyboard_interfaces;
} aether_usb_host_status_t;
esp_err_t app_otg_get_status(aether_usb_host_status_t *status);
