#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <string.h>
#include <strings.h>
#include "esp_err.h"

typedef enum {
    APP_IMAGE_UNKNOWN, APP_IMAGE_JPEG, APP_IMAGE_BMP, APP_IMAGE_PNG, APP_IMAGE_GIF,
} app_gallery_format_t;

#define APP_GALLERY_RASTER_MAX_FILE (16U * 1024U * 1024U)

static inline app_gallery_format_t app_gallery_format(const char *path)
{
    const char *ext = strrchr(path, '.');
    if (!ext) return APP_IMAGE_UNKNOWN;
    if (!strcasecmp(ext, ".jpg") || !strcasecmp(ext, ".jpeg")) return APP_IMAGE_JPEG;
    if (!strcasecmp(ext, ".bmp")) return APP_IMAGE_BMP;
    if (!strcasecmp(ext, ".png")) return APP_IMAGE_PNG;
    if (!strcasecmp(ext, ".gif")) return APP_IMAGE_GIF;
    return APP_IMAGE_UNKNOWN;
}

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

static inline uint32_t app_gallery_be32(const uint8_t *p)
{
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}

/* Fixed headers keep the catalog scan cheap. The worker validates the payload. */
static inline esp_err_t app_gallery_raster_header(app_gallery_format_t format, const uint8_t *header,
                                                  size_t bytes, unsigned *width, unsigned *height)
{
    unsigned limit;
    if (format == APP_IMAGE_PNG) {
        if (bytes < 33 || memcmp(header, "\x89PNG\r\n\x1a\n\0\0\0\rIHDR", 16) != 0)
            return ESP_ERR_INVALID_RESPONSE;
        *width = app_gallery_be32(header + 16);
        *height = app_gallery_be32(header + 20);
        const unsigned depth = header[24], color = header[25];
        const bool packed = depth == 1 || depth == 2 || depth == 4;
        const bool normal_depth = depth == 8 || depth == 16;
        if (header[26] || header[27] || header[28] > 1 ||
            !((color == 0 && (packed || normal_depth)) ||
              (color == 3 && (packed || depth == 8)) ||
              ((color == 2 || color == 4 || color == 6) && normal_depth))) return ESP_ERR_NOT_SUPPORTED;
        limit = depth == 16 ? 2U * 1024U * 1024U : 4U * 1024U * 1024U;
    } else {
        if (bytes < 13 || (memcmp(header, "GIF87a", 6) != 0 && memcmp(header, "GIF89a", 6) != 0))
            return ESP_ERR_INVALID_RESPONSE;
        *width = app_gallery_le16(header + 6);
        *height = app_gallery_le16(header + 8);
        limit = 1024U * 1024U;
    }
    if (!*width || !*height || *width > 8192 || *height > 8192 || *width * *height > limit)
        return ESP_ERR_INVALID_SIZE;
    return ESP_OK;
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
