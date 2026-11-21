#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif
typedef struct aether_mp4_audio *aether_mp4_audio_handle_t;
typedef struct {
    uint32_t sample_rate,duration_ms,position_ms;
    uint8_t channels,object_type,asc[64];
    size_t asc_size;
    bool finished;
    esp_err_t last_error;
} aether_mp4_audio_info_t;
/** Pull interface, used by one owner task. FILE is borrowed: close does not
 * fclose it. Validates the selected AAC-LC audio track independently of video.
 * A failed open leaves out NULL and a human-readable reason in detail. */
esp_err_t aether_mp4_audio_open(FILE *,bool (*cancel)(void *),void *context,
                               aether_mp4_audio_handle_t *out,char *detail,size_t detail_size);
esp_err_t aether_mp4_audio_get_info(aether_mp4_audio_handle_t,aether_mp4_audio_info_t *);
/** Always interleaved PCM16 stereo at 48000 Hz; max_frames counts stereo
 * frames, not scalar samples. 0 + ESP_OK means EOF, 0 + error means failure. */
size_t aether_mp4_audio_read(aether_mp4_audio_handle_t,int16_t *stereo,size_t max_frames,esp_err_t *error);
/** Rebuild AAC overlap using one preceding access unit and trim to target. */
esp_err_t aether_mp4_audio_seek(aether_mp4_audio_handle_t,uint32_t position_ms);
void aether_mp4_audio_close(aether_mp4_audio_handle_t);
#ifdef __cplusplus
}
#endif
