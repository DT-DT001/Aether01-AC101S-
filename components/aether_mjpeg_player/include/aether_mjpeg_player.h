#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "aether_storage.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct aether_mjpeg_player_t *aether_mjpeg_player_handle_t;

typedef enum {
    AETHER_MJPEG_STOPPED = 0,
    AETHER_MJPEG_STARTING,
    AETHER_MJPEG_PLAYING,
    AETHER_MJPEG_PAUSED,
    AETHER_MJPEG_FINISHED,
    AETHER_MJPEG_ERROR,
} aether_mjpeg_state_t;

typedef struct {
    aether_mjpeg_state_t state;
    esp_err_t last_error;
    uint32_t frame_number;
    uint16_t width;
    uint16_t height;
    uint8_t fps; /**< Effective target FPS, rounded for fractional AVI cadence. */
    uint32_t actual_fps_x100; /**< Produced frames per second; not measured panel scan-out FPS. */
    uint32_t source_fps_x100; /**< AVI main-header cadence, zero for raw/unknown streams. */
    uint32_t read_us;
    uint32_t decode_us;
    uint32_t callback_us;
    uint32_t late_frames;
    uint32_t elapsed_ms; /**< Active playback time, excluding pause. */
    char path[192];
} aether_mjpeg_status_t;

/** Frame memory is borrowed and guaranteed only until the callback returns. */
typedef void (*aether_mjpeg_frame_cb_t)(const uint16_t *pixels,
                                        uint16_t width, uint16_t height,
                                        uint32_t frame_number,
                                        void *user_ctx);

esp_err_t aether_mjpeg_player_new(aether_storage_handle_t storage,
                                  aether_mjpeg_player_handle_t *out_handle);

/**
 * Play a raw MJPEG stream or classic RIFF MJPEG-in-AVI file (first video
 * stream; audio tracks and multi-RIFF OpenDML continuation are unsupported).
 * Frames must be baseline JPEG,
 * no larger than 480x480, with dimensions aligned to 16 pixels. fps=0 selects
 * AVI's native cadence (1..30 FPS), or 24 FPS for raw streams with no timing
 * metadata. Explicit fps=1..30 overrides the source cadence. Unsupported
 * automatic cadence is reported asynchronously as ERROR/NOT_SUPPORTED.
 */
esp_err_t aether_mjpeg_player_play(aether_mjpeg_player_handle_t handle,
                                   const char *path, uint8_t fps,
                                   aether_mjpeg_frame_cb_t frame_cb,
                                   void *user_ctx);
esp_err_t aether_mjpeg_player_set_paused(aether_mjpeg_player_handle_t handle,
                                         bool paused);
void aether_mjpeg_player_set_screen_paused(aether_mjpeg_player_handle_t handle,
                                         bool paused);
esp_err_t aether_mjpeg_player_set_fps(aether_mjpeg_player_handle_t handle, uint8_t fps);
esp_err_t aether_mjpeg_player_stop(aether_mjpeg_player_handle_t handle);
/** Stop and wait for callbacks and hardware accesses to finish. Zero polls. */
esp_err_t aether_mjpeg_player_stop_wait(aether_mjpeg_player_handle_t handle,
                                        uint32_t timeout_ms);
/** Release the retained last-frame buffer when its UI page is closed. */
void aether_mjpeg_player_release_frame(aether_mjpeg_player_handle_t handle);
esp_err_t aether_mjpeg_player_get_status(aether_mjpeg_player_handle_t handle,
                                         aether_mjpeg_status_t *status);
esp_err_t aether_mjpeg_player_del(aether_mjpeg_player_handle_t handle);

#ifdef __cplusplus
}
#endif
