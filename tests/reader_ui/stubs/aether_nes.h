#pragma once
typedef void *aether_nes_handle_t;
#include "esp_err.h"
#include <stdint.h>
typedef enum {AETHER_NES_STOPPED, AETHER_NES_STARTING, AETHER_NES_RUNNING,
              AETHER_NES_PAUSED, AETHER_NES_ERROR} aether_nes_state_t;
typedef struct { aether_nes_state_t state; } aether_nes_status_t;
esp_err_t aether_nes_get_status(aether_nes_handle_t, aether_nes_status_t *);
esp_err_t aether_nes_stop_wait(aether_nes_handle_t, uint32_t);
