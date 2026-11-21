#pragma once
typedef void *aether_mjpeg_player_handle_t;
#include "esp_err.h"
#include <stdint.h>
typedef enum {AETHER_MJPEG_STOPPED, AETHER_MJPEG_STARTING, AETHER_MJPEG_PLAYING,
              AETHER_MJPEG_PAUSED, AETHER_MJPEG_FINISHED, AETHER_MJPEG_ERROR} aether_mjpeg_state_t;
typedef struct { aether_mjpeg_state_t state; } aether_mjpeg_status_t;
esp_err_t aether_mjpeg_player_get_status(aether_mjpeg_player_handle_t, aether_mjpeg_status_t *);
esp_err_t aether_mjpeg_player_stop_wait(aether_mjpeg_player_handle_t, uint32_t);
