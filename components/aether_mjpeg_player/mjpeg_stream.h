#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include "esp_err.h"

#define AETHER_MJPEG_READ_BYTES 4096U

typedef struct {
    FILE *file;
    uint8_t *buffer;
    size_t position;
    size_t length;
    unsigned refill_count;
    uint64_t offset;
    uint64_t read_limit;
    uint64_t list_end[8];
    uint64_t list_padded_end[8];
    bool list_movi[8];
    uint8_t depth;
    bool initialized;
    bool avi;
    bool video_stream_selected;
    uint16_t video_stream_id;
    uint32_t frame_period_us;
    bool (*cancelled)(void *context);
    void (*yield)(void *context);
    void *context;
} aether_mjpeg_reader_t;

/* Retains bytes read beyond EOI for the next frame. JPEG segment lengths are
 * honored, so marker-like bytes in EXIF/APP/COM payloads cannot truncate a frame. */
esp_err_t aether_mjpeg_read_frame(aether_mjpeg_reader_t *reader, uint8_t *frame,
                                  size_t capacity, size_t *frame_size);
