#pragma once
typedef void *aether_video_player_handle_t;
#include "esp_err.h"
#include <stdint.h>
typedef enum {AETHER_VIDEO_STOPPED, AETHER_VIDEO_STARTING, AETHER_VIDEO_PLAYING,
              AETHER_VIDEO_PAUSED, AETHER_VIDEO_FINISHED, AETHER_VIDEO_ERROR} aether_video_state_t;
typedef struct { aether_video_state_t state; } aether_video_status_t;
esp_err_t aether_video_player_get_status(aether_video_player_handle_t, aether_video_status_t *);
esp_err_t aether_video_player_stop_wait(aether_video_player_handle_t, uint32_t);
