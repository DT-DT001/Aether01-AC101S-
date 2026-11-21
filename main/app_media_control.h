#pragma once

#include "aether_audio_player.h"
#include "aether_video_player.h"
#include "aether_nes.h"
#include "aether_bluetooth.h"

/* Call from the GUI service after processing pending media handovers. Backend
 * calls only publish commands; no filesystem/codec operation runs here. */
esp_err_t app_media_control_dispatch(aether_audio_player_handle_t audio,
    aether_video_player_handle_t video, aether_nes_handle_t nes,
    aether_bluetooth_remote_action_t action);

/* An unavailable snapshot is busy: never authorize a Flash commit on an
 * assumption that a temporarily locked media backend has stopped. */
bool app_media_control_flash_busy(aether_audio_player_handle_t audio,
    aether_video_player_handle_t video, aether_nes_handle_t nes);
