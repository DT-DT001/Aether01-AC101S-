#pragma once

#include <stdint.h>
#include "ac101s_audio.h"

#ifdef __cplusplus
extern "C" {
#endif

/* IMA ADPCM, mono, 16 kHz. Each 20 ms packet starts with a little-endian
 * PCM16 predictor and an unsigned step index. The remaining samples are
 * packed in low-nibble-first order, so packets decode independently. */
#define AETHER_INTERCOM_AUDIO_FRAME_MS 20
#define AETHER_INTERCOM_AUDIO_FRAME_BYTES 163

typedef struct aether_intercom_audio *aether_intercom_audio_handle_t;

typedef enum {
    AETHER_INTERCOM_AUDIO_CAPTURE,
    AETHER_INTERCOM_AUDIO_PLAYBACK,
} aether_intercom_audio_mode_t;

/* Use on one worker task. These functions may perform I/O or take a mutex. */
esp_err_t aether_intercom_audio_new(ac101s_audio_handle_t codec, aether_intercom_audio_handle_t *out);
esp_err_t aether_intercom_audio_del(aether_intercom_audio_handle_t h);
/* A failed start can hold partial codec state. Stop must succeed before
 * deleting this handle or retrying start. Stop is safe to retry. */
esp_err_t aether_intercom_audio_start(aether_intercom_audio_handle_t h, aether_intercom_audio_mode_t mode);
esp_err_t aether_intercom_audio_stop(aether_intercom_audio_handle_t h);
/* Allow at most 60 ms to acquire the codec mutex and complete each PCM block.
 * Discard the captured frame on error. Bad step indices return INVALID_ARG. */
esp_err_t aether_intercom_audio_capture(aether_intercom_audio_handle_t h,
                                       uint8_t frame[AETHER_INTERCOM_AUDIO_FRAME_BYTES]);
esp_err_t aether_intercom_audio_play(aether_intercom_audio_handle_t h,
                                    const uint8_t frame[AETHER_INTERCOM_AUDIO_FRAME_BYTES]);
/* Replace one missing 20 ms packet with a fading continuation of recent
 * speech. Consecutive losses decay to silence. */
esp_err_t aether_intercom_audio_conceal(aether_intercom_audio_handle_t h);
esp_err_t aether_intercom_audio_silence(aether_intercom_audio_handle_t h);

#ifdef __cplusplus
}
#endif
