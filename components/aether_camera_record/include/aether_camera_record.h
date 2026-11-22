#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

struct aether_camera_record_index_page;

typedef struct {
    FILE *file;
    struct aether_camera_record_index_page *index_head;
    struct aether_camera_record_index_page *index_tail;
    uint32_t frame_count;
    uint32_t file_offset;
    uint32_t largest_frame;
    uint32_t nominal_period_us;
    uint16_t width;
    uint16_t height;
    bool short_write;
} aether_camera_record_t;

/* Initialize the recorder with {0}. Supply a newly opened seekable binary
 * file (wb), positioned at 0. All operations run on the owning writer task.
 * The caller is responsible for closing the file in every case. */
esp_err_t aether_camera_record_start(aether_camera_record_t *record, FILE *file,
                                    uint16_t width, uint16_t height,
                                    uint32_t frame_interval_100ns);

/* Store one complete JPEG from the configured stream. At the AVI file or
 * index size limit, returns ESP_ERR_INVALID_SIZE before modifying the file. */
esp_err_t aether_camera_record_write(aether_camera_record_t *record, const void *jpeg, size_t size);

/* Optional capture_duration_us includes the duration of the last frame.
 * A zero duration uses the nominal UVC interval. Always releases index
 * memory. Returns ESP_ERR_NOT_FOUND for recordings with no complete frame.
 * Treat the file as saved only if both finish and fclose succeeded. */
esp_err_t aether_camera_record_finish(aether_camera_record_t *record, uint64_t capture_duration_us);

/* Release index memory without touching the file, for example on card removal. */
void aether_camera_record_abort(aether_camera_record_t *record);

#ifdef __cplusplus
}
#endif
