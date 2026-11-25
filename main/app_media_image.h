#pragma once

#include <stdbool.h>
#include <stdatomic.h>
#include "aether_storage.h"

typedef struct {
    uint16_t *pixels;
    uint16_t width, height;
} app_media_image_t;

/* Worker only. Acquire and close the source lease internally. On success the
 * caller owns fitted RGB565 pixels. With fill_bounds, small images also scale
 * up to the largest size that fits their aspect ratio in the given bounds. */
esp_err_t app_media_image_read(aether_storage_handle_t storage, const char *path,
                               uint32_t generation, const atomic_bool *cancelled,
                               uint16_t max_width, uint16_t max_height, bool fill_bounds,
                               app_media_image_t *image);
