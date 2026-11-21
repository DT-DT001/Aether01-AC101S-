#pragma once
#include <stdint.h>
#include "esp_err.h"
typedef struct fake_nes *aether_nes_handle_t;
esp_err_t aether_nes_set_external_input(aether_nes_handle_t, uint16_t);
