#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

typedef struct aether_mp3_stream aether_mp3_stream_t;
typedef bool (*aether_mp3_cancel_cb_t)(void *context);

/* Sole decoder-owner diagnostics; logical read time includes fread blocking,
 * not an SD hardware underrun counter. clear_metrics never changes playback. */
typedef struct {
    uint64_t read_total_us;
    uint32_t read_calls,refill_max_bytes,refill_max_us,io_buffer_bytes;
} aether_mp3_metrics_t;
void aether_mp3_get_metrics(const aether_mp3_stream_t *,aether_mp3_metrics_t *);
void aether_mp3_clear_metrics(aether_mp3_stream_t *);

esp_err_t aether_mp3_open(FILE *file, aether_mp3_cancel_cb_t cancel, void *context,
                          aether_mp3_stream_t **out_stream);
void aether_mp3_close(aether_mp3_stream_t *stream);
uint32_t aether_mp3_rate(const aether_mp3_stream_t *stream);
uint16_t aether_mp3_channels(const aether_mp3_stream_t *stream);
uint32_t aether_mp3_duration_ms(const aether_mp3_stream_t *stream);
uint32_t aether_mp3_position_ms(const aether_mp3_stream_t *stream);
esp_err_t aether_mp3_seek(aether_mp3_stream_t *stream, uint32_t position_ms);
size_t aether_mp3_read(aether_mp3_stream_t *stream, int16_t *output,
                       size_t frames, esp_err_t *error);
