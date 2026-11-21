#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "esp_err.h"

#define MP4_AAC_OUTPUT_RATE 48000U
#define MP4_AAC_OUTPUT_BLOCK_FRAMES 1024U
#define MP4_AAC_MAX_ACCESS_UNIT 8192U

typedef struct mp4_aac *mp4_aac_handle_t;
typedef bool (*mp4_aac_cancel_cb_t)(void *context);
/* PCM16 interleaved stereo at 48kHz. Valid only during this callback. */
typedef esp_err_t (*mp4_aac_pcm_cb_t)(const int16_t *samples, size_t frames, void *context);

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
} mp4_aac_config_t;

typedef struct {
    uint32_t sample_rate;
    uint8_t channels;
    uint64_t access_units;
    uint64_t input_frames;
    uint64_t output_frames; /* Successfully accepted by the PCM callback. */
    esp_err_t last_error;
    bool finished;
} mp4_aac_status_t;

/* AAC-LC AOT2, 8..48k standard sample rates, mono/stereo, 1024-sample
 * GASpecificConfig only. Accepts the optional explicit "SBR absent" extension.
 * Does not guess HE-AAC, PCE/channelConfig0, 960-frame or ER/ELD settings. */
esp_err_t mp4_aac_parse_asc(const uint8_t *asc, size_t length, mp4_aac_config_t *config);
esp_err_t mp4_aac_new(const uint8_t *asc, size_t length, mp4_aac_cancel_cb_t cancel,
                      void *cancel_context, mp4_aac_handle_t *out_handle);
/* Supply one complete MP4 AAC access unit. Cross-AU resampling phase/history
 * are retained; callback blocks contain at most 1024 frames. Errors latch
 * until reset/del, preventing retrying a partially delivered access unit. */
esp_err_t mp4_aac_decode(mp4_aac_handle_t handle, const uint8_t *access_unit, size_t length,
                         mp4_aac_pcm_cb_t callback, void *context);
/* Normal EOF only: emit the final interpolation endpoint exactly once.
 * Do not finish on stop/removal/error; discard by deleting/resetting instead. */
esp_err_t mp4_aac_finish(mp4_aac_handle_t handle, mp4_aac_pcm_cb_t callback, void *context);
/* One serialized owner at a time, including status/reset/del. No method is
 * safe concurrently with decode or callback execution. A retained video
 * worker owns all segments; final parent cleanup takes ownership only after
 * that worker publishes its separate final-exit barrier. */
esp_err_t mp4_aac_reset(mp4_aac_handle_t handle);
esp_err_t mp4_aac_get_status(mp4_aac_handle_t handle, mp4_aac_status_t *status);
void mp4_aac_del(mp4_aac_handle_t handle);
