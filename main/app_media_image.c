#include "app_media_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "app_gallery_format.h"
#include "aether_image.h"
#include "aether_media_fs.h"
#include "esp_heap_caps.h"

typedef struct {
    aether_storage_handle_t storage;
    const char *path;
    uint32_t generation;
    const atomic_bool *cancelled;
    aether_media_fs_lease_t lease;
} image_source_t;

static bool source_available(const image_source_t *source)
{
    return !atomic_load(source->cancelled) && aether_media_fs_available(&source->lease) &&
           aether_media_fs_generation(source->storage, source->path) == source->generation;
}

static esp_err_t read_jpeg(FILE *file, size_t bytes, const image_source_t *source,
                          uint16_t max_width, uint16_t max_height, app_media_image_t *image)
{
    if (bytes > AETHER_IMAGE_MAX_JPEG_BYTES) return ESP_ERR_INVALID_SIZE;
    uint8_t *data = heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!data) return ESP_ERR_NO_MEM;
    size_t offset = 0;
    esp_err_t error = ESP_OK;
    while (offset < bytes && source_available(source)) {
        const size_t count = bytes - offset < 32768U ? bytes - offset : 32768U;
        if (fread(data + offset, 1, count, file) != count) { error = ESP_FAIL; break; }
        offset += count;
    }
    if (error == ESP_OK && !source_available(source)) error = ESP_ERR_INVALID_STATE;
    aether_image_decoder_t *decoder = NULL;
    if (error == ESP_OK) error = aether_image_decoder_new(&decoder);
    if (error == ESP_OK) {
        error = aether_image_decode_jpeg(decoder, data, bytes, image->pixels,
                                        max_width, max_height, &image->width, &image->height);
    }
    aether_image_decoder_del(decoder);
    free(data);
    return error;
}

static esp_err_t read_bmp(FILE *file, size_t bytes, const image_source_t *source,
                         uint16_t max_width, uint16_t max_height, app_media_image_t *image)
{
    uint8_t header[54];
    if (bytes < sizeof(header) || fread(header, 1, sizeof(header), file) != sizeof(header))
        return ESP_ERR_INVALID_RESPONSE;
    app_gallery_bmp_t bmp;
    esp_err_t error = app_gallery_bmp_header(header, bytes, &bmp);
    if (error != ESP_OK) return error;
    unsigned width = bmp.width < max_width ? bmp.width : max_width;
    unsigned height = bmp.height * width / bmp.width;
    if (height > max_height) {
        height = max_height;
        width = bmp.width * height / bmp.height;
    }
    if (!width) width = 1;
    if (!height) height = 1;
    uint8_t *row = heap_caps_malloc(bmp.stride, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!row) return ESP_ERR_NO_MEM;

    const unsigned x_step = (bmp.width << 16) / width;
    const unsigned y_step = (bmp.height << 16) / height;
    unsigned source_y = y_step / 2;
    for (unsigned y = 0; y < height; ++y, source_y += y_step) {
        if (!source_available(source)) { error = ESP_ERR_INVALID_STATE; break; }
        const unsigned offset = bmp.offset + (source_y >> 16) * bmp.stride;
        if (fseek(file, (long)offset, SEEK_SET) || fread(row, 1, bmp.stride, file) != bmp.stride) {
            error = ESP_FAIL;
            break;
        }
        /* Read physical rows forwards even for bottom-up BMPs. */
        uint16_t *dest = image->pixels + (bmp.top_down ? y : height - y - 1) * width;
        unsigned source_x = x_step / 2;
        for (unsigned x = 0; x < width; ++x, source_x += x_step) {
            const uint8_t *src = row + (source_x >> 16) * bmp.pixel_bytes;
            dest[x] = (uint16_t)((src[2] >> 3) << 11 | (src[1] >> 2) << 5 | src[0] >> 3);
        }
    }
    free(row);
    image->width = width;
    image->height = height;
    return error;
}

static esp_err_t decode_image(const image_source_t *source, uint16_t max_width,
                              uint16_t max_height, app_media_image_t *image)
{
    if (!source_available(source)) return ESP_ERR_INVALID_STATE;
    FILE *file = fopen(source->path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    esp_err_t error = ESP_ERR_INVALID_SIZE;
    const long bytes = fseek(file, 0, SEEK_END) == 0 ? ftell(file) : -1;
    if (bytes > 0 && fseek(file, 0, SEEK_SET) == 0) {
        image->pixels = heap_caps_malloc((size_t)max_width * max_height * sizeof(uint16_t),
                                        MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        const char *ext = strrchr(source->path, '.');
        if (!image->pixels) error = ESP_ERR_NO_MEM;
        else if (ext && !strcasecmp(ext, ".bmp"))
            error = read_bmp(file, (size_t)bytes, source, max_width, max_height, image);
        else error = read_jpeg(file, (size_t)bytes, source, max_width, max_height, image);
    }
    fclose(file);
    return error;
}

static void upscale_image(app_media_image_t *image, uint16_t max_width, uint16_t max_height)
{
    const unsigned source_width = image->width;
    const unsigned source_height = image->height;
    if (source_width == max_width || source_height == max_height) return;

    unsigned width = max_width, height = max_height;
    if (source_height * max_width > source_width * max_height)
        width = (source_width * max_height + source_height / 2U) / source_height;
    else
        height = (source_height * max_width + source_width / 2U) / source_width;

    const unsigned x_step = (source_width << 16) / width;
    const unsigned y_step = (source_height << 16) / height;
    /* Each sampled source pixel is at or before its destination because both
     * axes grow. Walking backwards preserves unread pixels in the buffer. */
    for (unsigned y = height; y-- > 0;) {
        const unsigned source_y = (y * y_step + y_step / 2U) >> 16;
        const uint16_t *source = image->pixels + source_y * source_width;
        uint16_t *dest = image->pixels + y * width;
        for (unsigned x = width; x-- > 0;) {
            const unsigned source_x = (x * x_step + x_step / 2U) >> 16;
            dest[x] = source[source_x];
        }
    }
    image->width = width;
    image->height = height;
}

esp_err_t app_media_image_read(aether_storage_handle_t storage, const char *path,
                               uint32_t generation, const atomic_bool *cancelled,
                               uint16_t max_width, uint16_t max_height, bool fill_bounds,
                               app_media_image_t *image)
{
    *image = (app_media_image_t){0};
    if (!max_width || max_width > 480 || !max_height || max_height > 480 ||
        !aether_media_fs_safe_path(path)) return ESP_ERR_INVALID_ARG;
    if (atomic_load(cancelled)) return ESP_ERR_INVALID_STATE;
    image_source_t source = { .storage = storage, .path = path, .generation = generation, .cancelled = cancelled };
    esp_err_t error = aether_media_fs_acquire(storage, path, &source.lease);
    if (error == ESP_OK) {
        error = decode_image(&source, max_width, max_height, image);
        if (error == ESP_OK && (!image->width || image->width > max_width ||
                                !image->height || image->height > max_height)) error = ESP_ERR_INVALID_SIZE;
        if (error == ESP_OK && fill_bounds) upscale_image(image, max_width, max_height);
        if (error == ESP_OK && !source_available(&source)) error = ESP_ERR_INVALID_STATE;
        aether_media_fs_release(&source.lease);
    }
    if (error != ESP_OK) {
        free(image->pixels);
        *image = (app_media_image_t){0};
    }
    return error;
}
