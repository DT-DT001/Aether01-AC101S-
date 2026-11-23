#pragma once
#include "aether_mjpeg_player.h"
#include "ac101s_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aether_mp4_player *aether_mp4_player_handle_t;
typedef aether_mjpeg_frame_cb_t aether_mp4_frame_cb_t;
typedef struct {
    aether_mjpeg_state_t state;
    esp_err_t last_error;
    uint32_t frame_number;
    uint16_t width, height;
    uint8_t fps;
    uint32_t actual_fps_x100, source_fps_x100;
    uint32_t read_us, decode_us, callback_us, late_frames, elapsed_ms;
    uint32_t read_max_us,decode_max_us,callback_max_us;
    uint32_t h264_us,h264_max_us,convert_us,convert_max_us; /* Per-feed SDK/colour-conversion totals. */
    uint64_t h264_total_us,convert_total_us; /* Session sums, including decoder priming/preroll. */
    int32_t video_affinity,audio_affinity; /* Native task affinity; tskNO_AFFINITY remains its SDK value. */
    uint32_t video_core_mask,audio_core_mask; /* Native sampled execution cores; zero on host. */
    uint32_t video_stack_addr,audio_stack_addr;
    bool video_stack_hp,audio_stack_hp; /* Native actual SP is both internal and DMA-capable. */
    uint32_t video_stack_free_min_bytes,audio_stack_free_min_bytes; /* Native one-time final scan. */
    uint32_t h264_output_frames,converted_frames;
    uint32_t lag_rebuilds,lag_skip_burst_max;
    uint32_t presentation_drops; /* Expired displays only; H264 reference packets still decode. */
    char path[192];
    uint32_t duration_ms, position_ms;
    bool seek_supported;
    bool seeking;
    uint32_t seek_requested_ms;
    uint32_t seek_scan_us,seek_preroll_us;
    uint32_t audio_clock_wait_us,audio_clock_wait_max_us; /* Startup/resume excluded from presentation time. */
    uint32_t video_lookup_us,video_lookup_max_us;
    uint32_t audio_lookup_us,audio_lookup_max_us; /* Normal supply lookup only; seek scans have separate timings. */
    bool has_audio;
    uint32_t audio_rate;
    uint8_t audio_channels;
    esp_err_t audio_error;
    uint32_t audio_read_us,audio_read_max_us;
    uint32_t audio_decode_us,audio_decode_max_us; /* Excludes synchronous PCM output waits. */
    uint32_t audio_write_us,audio_write_max_us;
    uint32_t audio_supply_gap_us,audio_supply_gap_max_us; /* Last write return -> next entry. */
    uint32_t audio_late_units;

    uint32_t dropped_frames;
    uint8_t h264_profile;
    uint8_t h264_level;
    char detail[112]; /**< Actual format/decoder failure; display this with last_error. */
} aether_mp4_status_t;

/** audio may be NULL only for files without an audio track. Playback uses
 * native MP4 sample timing; arbitrary speed override is unsupported.
 * Video is avc1 constrained-baseline H.264, <=480x480 displayed pixels.
 * AAC-LC audio is decoded to 48kHz stereo. Other codecs fail explicitly. */
esp_err_t aether_mp4_player_new(aether_storage_handle_t storage,
                               ac101s_audio_handle_t audio,
                               aether_mp4_player_handle_t *out_handle);
esp_err_t aether_mp4_player_play(aether_mp4_player_handle_t handle,const char *path,
                                aether_mp4_frame_cb_t callback,void *context);
esp_err_t aether_mp4_player_set_paused(aether_mp4_player_handle_t handle,bool paused);
void aether_mp4_player_set_screen_paused(aether_mp4_player_handle_t handle,bool paused);
/** Nonblocking active-session seek. Rebuild from a preceding IDR and resync AAC.
 * Paused sessions publish one preview and stay paused. INVALID_STATE can mean
 * a short frame-publication gate is busy; retain the last GUI target and retry.
 * After ESP_OK no old-generation callback can start. Consumers must clear any
 * pending/image copy they received before this command. */
esp_err_t aether_mp4_player_seek(aether_mp4_player_handle_t handle,uint32_t position_ms);
/** Same command with a synchronous consumer reset before acceptance, protected
 * against both old and new frame publication. Runs on the calling thread only.
 * reset may clear an image/queued frame; it must not do I/O or reenter player
 * commands. Busy/error leaves reset uncalled. */
esp_err_t aether_mp4_player_seek_with_reset(aether_mp4_player_handle_t handle,uint32_t position_ms,
    void (*reset)(void *context),void *context);
esp_err_t aether_mp4_player_set_volume(aether_mp4_player_handle_t handle,uint8_t volume);
esp_err_t aether_mp4_player_stop(aether_mp4_player_handle_t handle);
esp_err_t aether_mp4_player_stop_wait(aether_mp4_player_handle_t handle,uint32_t timeout_ms);
void aether_mp4_player_release_frame(aether_mp4_player_handle_t handle);
esp_err_t aether_mp4_player_get_status(aether_mp4_player_handle_t handle,aether_mp4_status_t *status);
/** Destroy with exclusive ownership of the handle: no other API call may be
 * active, queued or start concurrently with del, including another del. On a
 * timeout the handle remains valid and retained workers may be retried. */
esp_err_t aether_mp4_player_del(aether_mp4_player_handle_t handle);

#ifdef __cplusplus
}
#endif
