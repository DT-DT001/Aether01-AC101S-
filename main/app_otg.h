#pragma once
#include "aether_board.h"
#include "aether_nes.h"
#include "app_otg_control.h"
#include "app_power.h"

/* Application-owned lazy host bridge. GUI calls only publish/read commands. */
esp_err_t app_otg_init(aether_board_t *board, aether_nes_handle_t nes, app_power_t *power);
