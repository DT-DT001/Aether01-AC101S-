#pragma once

#include <stdint.h>
#include "esp_partition.h"

enum { APP_WALLPAPER_SIZE = 480 };

typedef struct {
    const esp_partition_t *partition;
    uint32_t serial;
    uint8_t slot;
} app_wallpaper_store_t;

/* A valid empty store returns ESP_OK with NULL pixels, including after a reset. */
esp_err_t app_wallpaper_store_open(app_wallpaper_store_t *store, uint16_t **pixels,
                                   uint16_t *width, uint16_t *height);
/* NULL pixels with zero dimensions writes an empty record. Worker task only. */
esp_err_t app_wallpaper_store_write(app_wallpaper_store_t *store, const uint16_t *pixels,
                                    uint16_t width, uint16_t height);
