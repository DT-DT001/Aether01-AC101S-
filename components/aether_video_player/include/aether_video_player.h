#pragma once
#include "aether_mp4_player.h"

typedef struct aether_video_player *aether_video_player_handle_t;
typedef aether_mp4_status_t aether_video_status_t;
typedef aether_mjpeg_frame_cb_t aether_video_frame_cb_t;
#define AETHER_VIDEO_STOPPED AETHER_MJPEG_STOPPED
#define AETHER_VIDEO_STARTING AETHER_MJPEG_STARTING
#define AETHER_VIDEO_PLAYING AETHER_MJPEG_PLAYING
#define AETHER_VIDEO_PAUSED AETHER_MJPEG_PAUSED
#define AETHER_VIDEO_FINISHED AETHER_MJPEG_FINISHED
#define AETHER_VIDEO_ERROR AETHER_MJPEG_ERROR

esp_err_t aether_video_player_new(aether_storage_handle_t, ac101s_audio_handle_t,
                                  aether_video_player_handle_t *);
/* MP4 uses source timestamps; fps applies only to MJPEG/AVI. */
esp_err_t aether_video_player_play(aether_video_player_handle_t, const char *, uint8_t fps,
                                   aether_video_frame_cb_t, void *);
esp_err_t aether_video_player_set_paused(aether_video_player_handle_t, bool);
/* Active MP4 only; MJPEG returns NOT_SUPPORTED. No blocking decoder/file I/O. */
esp_err_t aether_video_player_seek(aether_video_player_handle_t,uint32_t position_ms);
/* GUI callers should clear copied frames in this synchronous reset callback;
 * it executes on the GUI caller under the producer publication gate, no I/O. */
esp_err_t aether_video_player_seek_with_reset(aether_video_player_handle_t,uint32_t position_ms,
    void (*reset)(void *context),void *context);
esp_err_t aether_video_player_set_fps(aether_video_player_handle_t, uint8_t);
esp_err_t aether_video_player_set_volume(aether_video_player_handle_t, uint8_t);
esp_err_t aether_video_player_stop(aether_video_player_handle_t);
esp_err_t aether_video_player_stop_wait(aether_video_player_handle_t, uint32_t);
void aether_video_player_release_frame(aether_video_player_handle_t);
esp_err_t aether_video_player_get_status(aether_video_player_handle_t, aether_video_status_t *);
esp_err_t aether_video_player_del(aether_video_player_handle_t);
