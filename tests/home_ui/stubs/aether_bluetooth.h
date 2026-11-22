#pragma once
#include <stdbool.h>
#include "esp_err.h"
typedef struct aether_bluetooth *aether_bluetooth_handle_t;
typedef struct { bool connected, enabled; } aether_bluetooth_status_t;
typedef struct { bool connected, requested; } aether_bluetooth_hid_status_t;
esp_err_t aether_bluetooth_get_status(aether_bluetooth_handle_t, aether_bluetooth_status_t *);
esp_err_t aether_bluetooth_hid_get_status(aether_bluetooth_handle_t, aether_bluetooth_hid_status_t *);
