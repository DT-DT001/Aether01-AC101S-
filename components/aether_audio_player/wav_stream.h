#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct {
    uint32_t sample_rate;
    uint16_t channels;
    uint32_t data_offset;
    uint32_t data_bytes;
    uint32_t frames;
} aether_wav_info_t;

/* All offsets and chunk sizes are checked against both RIFF and file bounds. */
esp_err_t aether_wav_parse(FILE *file, aether_wav_info_t *info);

typedef struct {
    FILE *file;
    aether_wav_info_t info;
    uint64_t phase;
    uint32_t first_frame;
    size_t buffered;
    int16_t input[1024 * 2];
} aether_wav_stream_t;

esp_err_t aether_wav_seek(aether_wav_stream_t *stream, uint32_t position_ms);
/* Linear interpolation converts 8..48 kHz mono/stereo PCM16 to 48 kHz stereo.
 * Returns frames, zero at EOF, and sets error on a short read. */
size_t aether_wav_read(aether_wav_stream_t *stream, int16_t *output,
                       size_t frames, esp_err_t *error);
uint32_t aether_wav_position_ms(const aether_wav_stream_t *stream);
void aether_pcm_gain(int16_t *samples, size_t sample_count, int volume);
