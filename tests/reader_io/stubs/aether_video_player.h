#pragma once
#include "../../reader_ui/stubs/aether_video_player.h"
esp_err_t aether_video_player_stop(aether_video_player_handle_t);
void aether_video_player_release_frame(aether_video_player_handle_t);
