#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

typedef struct {
    uint32_t offset, width, height, stride, pixel_bytes;
    bool top_down;
} app_gallery_bmp_t;

static inline uint16_t app_gallery_le16(const uint8_t *p)
{
    return (uint16_t)((uint16_t)p[0] | (uint16_t)p[1] << 8);
}

static inline uint32_t app_gallery_le32(const uint8_t *p)
{
    return (uint32_t)app_gallery_le16(p) | (uint32_t)app_gallery_le16(p + 2) << 16;
}

/* Shared by the catalog header check and the viewer's BMP decoder. */
static inline esp_err_t app_gallery_bmp_header(const uint8_t header[54], uint64_t bytes,
                                               app_gallery_bmp_t *bmp)
{
    const uint32_t dib_size = app_gallery_le32(header + 14);
    const uint32_t offset = app_gallery_le32(header + 10);
    const unsigned bits = app_gallery_le16(header + 28);
    const int32_t width = (int32_t)app_gallery_le32(header + 18);
    const int32_t raw_height = (int32_t)app_gallery_le32(header + 22);
    if (header[0] != 'B' || header[1] != 'M' || dib_size < 40 || app_gallery_le16(header + 26) != 1 ||
        app_gallery_le32(header + 30) != 0 || (bits != 24 && bits != 32)) return ESP_ERR_NOT_SUPPORTED;
    if (width <= 0 || width > 8192 || !raw_height || raw_height < -8192 || raw_height > 8192 ||
        offset < 14U + (uint64_t)dib_size) return ESP_ERR_INVALID_SIZE;
    const unsigned height = (unsigned)(raw_height < 0 ? -raw_height : raw_height);
    const unsigned pixel_bytes = bits / 8;
    const unsigned stride = ((unsigned)width * pixel_bytes + 3U) & ~3U;
    if (offset + (uint64_t)stride * height > bytes) return ESP_ERR_INVALID_SIZE;
    *bmp = (app_gallery_bmp_t){offset, (unsigned)width, height, stride, pixel_bytes, raw_height < 0};
    return ESP_OK;
}
