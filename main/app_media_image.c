#include "app_media_image.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "app_gallery_format.h"
#include "aether_image.h"
#include "aether_media_fs.h"
#include "esp_heap_caps.h"

#define IMAGE_PSRAM (MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)
#define RASTER_ALLOCATION_LIMIT (20U * 1024U * 1024U)

static void *raster_alloc(size_t bytes)
{
    return bytes <= RASTER_ALLOCATION_LIMIT ? heap_caps_malloc(bytes, IMAGE_PSRAM) : NULL;
}

static void *raster_realloc(void *buffer, size_t bytes)
{
    return bytes <= RASTER_ALLOCATION_LIMIT ? heap_caps_realloc(buffer, bytes, IMAGE_PSRAM) : NULL;
}

/* Reuse the stb_image copy shipped with the pinned LVGL dependency. Decode
 * single images only and keep its working buffers out of the internal heap. */
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#define STBI_ONLY_PNG
#define STBI_NO_GIF
#define STBI_NO_STDIO
#define STBI_NO_LINEAR
#define STBI_NO_SIMD
#define STBI_MAX_DIMENSIONS 8192
#define STBI_MALLOC(bytes) raster_alloc(bytes)
#define STBI_REALLOC(buffer, bytes) raster_realloc(buffer, bytes)
#define STBI_FREE(buffer) free(buffer)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#pragma GCC diagnostic ignored "-Wunused-parameter"
#include "src/libs/gltf/stb_image/stb_image.h"
#pragma GCC diagnostic pop

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

static void fit_image(unsigned sw, unsigned sh, unsigned max_width, unsigned max_height,
                      unsigned *width, unsigned *height)
{
    *width = sw < max_width ? sw : max_width;
    *height = sh * *width / sw;
    if (*height > max_height) {
        *height = max_height;
        *width = sw * *height / sh;
    }
    if (!*width) *width = 1;
    if (!*height) *height = 1;
}

typedef struct {
    FILE *file;
    const image_source_t *source;
    size_t remaining;
    bool failed;
} raster_input_t;

static int raster_read(void *context, char *out, int size)
{
    raster_input_t *input = context;
    if (input->failed || !input->remaining || size <= 0 || !source_available(input->source)) return 0;
    const size_t count = (size_t)size < input->remaining ? (size_t)size : input->remaining;
    const size_t actual = fread(out, 1, count, input->file);
    input->remaining -= actual;
    input->failed = actual != count;
    return (int)actual;
}

static void raster_skip(void *context, int size)
{
    raster_input_t *input = context;
    if (input->failed || !size) return;
    if (size < 0 || (size_t)size > input->remaining || !source_available(input->source) ||
        fseek(input->file, size, SEEK_CUR)) {
        input->failed = true;
        return;
    }
    input->remaining -= (size_t)size;
}

static int raster_eof(void *context)
{
    const raster_input_t *input = context;
    return input->failed || !input->remaining || !source_available(input->source);
}

static esp_err_t read_png(FILE *file, size_t bytes, const image_source_t *source,
                         uint16_t max_width, uint16_t max_height, app_media_image_t *image)
{
    if (bytes > APP_GALLERY_RASTER_MAX_FILE) return ESP_ERR_INVALID_SIZE;
    uint8_t header[33];
    const size_t header_bytes = fread(header, 1, sizeof(header), file);
    unsigned sw, sh;
    const esp_err_t checked = app_gallery_raster_header(APP_IMAGE_PNG, header, header_bytes, &sw, &sh);
    if (checked != ESP_OK) return checked;
    if (fseek(file, 0, SEEK_SET)) return ESP_FAIL;

    const stbi_io_callbacks callbacks = {raster_read, raster_skip, raster_eof};
    raster_input_t input = {.file = file, .source = source, .remaining = bytes};
    int w = 0, h = 0;
    uint8_t *rgba = stbi_load_from_callbacks(&callbacks, &input, &w, &h, NULL, 4);
    const char *failure = rgba ? NULL : stbi_failure_reason();
    esp_err_t error = failure && !strcmp(failure, "outofmem") ? ESP_ERR_NO_MEM : ESP_ERR_INVALID_RESPONSE;
    if (rgba && !input.failed && w == (int)sw && h == (int)sh) {
        unsigned width, height;
        fit_image(sw, sh, max_width, max_height, &width, &height);
        const uint32_t dx = (sw << 16) / width, dy = (sh << 16) / height;
        uint16_t *dest = image->pixels;
        unsigned source_y = dy / 2;
        for (unsigned y = 0; y < height; ++y, source_y += dy) {
            if (!source_available(source)) break;
            const uint8_t *row = rgba + (source_y >> 16) * sw * 4U;
            unsigned source_x = dx / 2;
            for (unsigned x = 0; x < width; ++x, source_x += dx) {
                const uint8_t *p = row + (source_x >> 16) * 4U;
                /* Composite transparent pixels on black for RGB565 and wallpaper storage. */
                const unsigned a = p[3];
                const unsigned r = (p[0] * a + 127U) / 255U;
                const unsigned g = (p[1] * a + 127U) / 255U;
                const unsigned b = (p[2] * a + 127U) / 255U;
                *dest++ = (uint16_t)((r >> 3) << 11 | (g >> 2) << 5 | b >> 3);
            }
        }
        if (dest == image->pixels + width * height) {
            image->width = width;
            image->height = height;
            error = ESP_OK;
        }
    }
    stbi_image_free(rgba);
    return source_available(source) ? error : ESP_ERR_INVALID_STATE;
}

typedef struct {
    uint16_t prefix[4096];
    uint8_t suffix[4096], chain[4096];
    uint8_t block[255];
    unsigned length, position, bits;
    uint32_t pending;
} gif_codes_t;

static int gif_next_code(raster_input_t *input, gif_codes_t *codes, unsigned size)
{
    while (codes->bits < size) {
        if (codes->position == codes->length) {
            uint8_t length;
            if (raster_read(input, (char *)&length, 1) != 1 || !length ||
                raster_read(input, (char *)codes->block, length) != length) return -1;
            codes->length = length;
            codes->position = 0;
        }
        codes->pending |= (uint32_t)codes->block[codes->position++] << codes->bits;
        codes->bits += 8;
    }
    const int code = (int)(codes->pending & ((1U << size) - 1U));
    codes->pending >>= size;
    codes->bits -= size;
    return code;
}

static bool gif_palette(raster_input_t *input, uint16_t *palette, unsigned count)
{
    uint8_t rgb[768];
    if (raster_read(input, (char *)rgb, (int)(count * 3U)) != (int)(count * 3U)) return false;
    for (unsigned i = 0; i < count; ++i) {
        const uint8_t *p = rgb + i * 3U;
        palette[i] = (uint16_t)((p[0] >> 3) << 11 | (p[1] >> 2) << 5 | p[2] >> 3);
    }
    return true;
}

static bool gif_frame_header(raster_input_t *input, uint8_t descriptor[9], int *transparent)
{
    uint8_t tag;
    while (raster_read(input, (char *)&tag, 1) == 1) {
        if (tag == 0x2c) return raster_read(input, (char *)descriptor, 9) == 9;
        uint8_t label, size;
        if (tag != 0x21 || raster_read(input, (char *)&label, 1) != 1 ||
            raster_read(input, (char *)&size, 1) != 1) return false;
        if (label == 0xf9) {
            uint8_t control[5];
            if (size != 4 || raster_read(input, (char *)control, 5) != 5 || control[4]) return false;
            *transparent = control[0] & 1U ? control[3] : -1;
        } else {
            /* A plain-text block consumes the preceding graphic control. */
            if (label == 0x01) *transparent = -1;
            while (size) {
                raster_skip(input, size);
                if (raster_read(input, (char *)&size, 1) != 1) return false;
            }
        }
    }
    return false;
}

static bool gif_pixels(raster_input_t *input, gif_codes_t *codes, uint8_t first_bits,
                       const uint8_t descriptor[9], const uint16_t *palette, unsigned color_count,
                       int transparent, unsigned stride, uint16_t *canvas)
{
    const unsigned width = app_gallery_le16(descriptor + 4), height = app_gallery_le16(descriptor + 6);
    const bool interlaced = (descriptor[8] & 0x40U) != 0;
    const unsigned clear = 1U << first_bits, end = clear + 1;
    unsigned next = end + 1, size = first_bits + 1, first = 0;
    int previous = -1;
    unsigned x = 0, y = 0, pass = 0, written = 0;
    static const uint8_t starts[] = {0, 4, 2, 1};
    static const uint8_t steps[] = {8, 8, 4, 2};
    for (;;) {
        const int code = gif_next_code(input, codes, size);
        if (code < 0 || (unsigned)code > next) return false;
        if ((unsigned)code == end) return written == width * height;
        if ((unsigned)code == clear) {
            next = end + 1;
            size = first_bits + 1;
            previous = -1;
            continue;
        }
        unsigned current = (unsigned)code, depth = 0;
        if (current == next) {
            if (previous < 0 || next == 4096) return false;
            codes->chain[depth++] = (uint8_t)first;
            current = (unsigned)previous;
        }
        while (current > end) {
            if (current >= next || depth >= sizeof(codes->chain) - 1) return false;
            codes->chain[depth++] = codes->suffix[current];
            current = codes->prefix[current];
        }
        if (current >= clear) return false;
        first = current;
        codes->chain[depth++] = (uint8_t)first;
        while (depth) {
            const unsigned index = codes->chain[--depth];
            if (y >= height || index >= color_count || ++written > width * height) return false;
            if ((int)index != transparent) canvas[y * stride + x] = palette[index];
            if (++x == width) {
                if (!source_available(input->source)) return false;
                x = 0;
                y += interlaced ? steps[pass] : 1U;
                while (interlaced && y >= height && pass < 3) y = starts[++pass];
            }
        }
        if (previous >= 0 && next < 4096) {
            codes->prefix[next] = (uint16_t)previous;
            codes->suffix[next++] = (uint8_t)first;
            if (next == (1U << size) && size < 12) ++size;
        }
        previous = code;
    }
}

/* Decode a single GIF frame. Iterative dictionary expansion bounds the stack
 * even when the file contains long LZW codes or repeated clear markers. */
static esp_err_t read_gif(FILE *file, size_t bytes, const image_source_t *source,
                          uint16_t max_width, uint16_t max_height, app_media_image_t *image)
{
    if (bytes > APP_GALLERY_RASTER_MAX_FILE) return ESP_ERR_INVALID_SIZE;
    raster_input_t input = {.file = file, .source = source, .remaining = bytes};
    uint8_t header[13], desc[9], first_bits;
    if (raster_read(&input, (char *)header, sizeof(header)) != sizeof(header)) return ESP_ERR_INVALID_RESPONSE;
    unsigned sw, sh;
    const esp_err_t checked = app_gallery_raster_header(APP_IMAGE_GIF, header, sizeof(header), &sw, &sh);
    if (checked != ESP_OK) return checked;
    uint16_t palette[256];
    unsigned count = header[10] & 0x80U ? 2U << (header[10] & 7U) : 0;
    if (count && !gif_palette(&input, palette, count)) return ESP_ERR_INVALID_RESPONSE;
    uint16_t background = header[11] < count ? palette[header[11]] : 0;
    int transparent = -1;
    if (!gif_frame_header(&input, desc, &transparent)) return ESP_ERR_INVALID_RESPONSE;
    const unsigned left = app_gallery_le16(desc), top = app_gallery_le16(desc + 2);
    const unsigned fw = app_gallery_le16(desc + 4), fh = app_gallery_le16(desc + 6);
    if (!fw || !fh || left + fw > sw || top + fh > sh) return ESP_ERR_INVALID_SIZE;
    if (desc[8] & 0x80U) {
        count = 2U << (desc[8] & 7U);
        if (!gif_palette(&input, palette, count)) return ESP_ERR_INVALID_RESPONSE;
    }
    if (!count || raster_read(&input, (char *)&first_bits, 1) != 1 || first_bits < 2 || first_bits > 8)
        return ESP_ERR_INVALID_RESPONSE;
    if (transparent >= 0) background = 0;
    gif_codes_t *codes = heap_caps_calloc(1, sizeof(*codes), IMAGE_PSRAM);
    uint16_t *canvas = heap_caps_malloc(sw * sh * sizeof(*canvas), IMAGE_PSRAM);
    esp_err_t error = ESP_ERR_NO_MEM;
    if (codes && canvas) {
        for (unsigned i = 0; i < sw * sh; ++i) canvas[i] = background;
        error = ESP_ERR_INVALID_RESPONSE;
        if (gif_pixels(&input, codes, first_bits, desc, palette, count, transparent, sw, canvas + top * sw + left)) {
            unsigned width, height;
            fit_image(sw, sh, max_width, max_height, &width, &height);
            const unsigned dx = (sw << 16) / width, dy = (sh << 16) / height;
            for (unsigned y = 0; y < height; ++y) {
                const uint16_t *row = canvas + (((y * dy + dy / 2) >> 16) * sw);
                for (unsigned x = 0; x < width; ++x)
                    image->pixels[y * width + x] = row[(x * dx + dx / 2) >> 16];
            }
            image->width = width;
            image->height = height;
            error = ESP_OK;
        }
    }
    free(canvas);
    free(codes);
    return source_available(source) ? error : ESP_ERR_INVALID_STATE;
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
    unsigned width, height;
    fit_image(bmp.width, bmp.height, max_width, max_height, &width, &height);
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
        const app_gallery_format_t format = app_gallery_format(source->path);
        if (!image->pixels) error = ESP_ERR_NO_MEM;
        else if (format == APP_IMAGE_BMP)
            error = read_bmp(file, (size_t)bytes, source, max_width, max_height, image);
        else if (format == APP_IMAGE_PNG)
            error = read_png(file, (size_t)bytes, source, max_width, max_height, image);
        else if (format == APP_IMAGE_GIF)
            error = read_gif(file, (size_t)bytes, source, max_width, max_height, image);
        else if (format == APP_IMAGE_JPEG)
            error = read_jpeg(file, (size_t)bytes, source, max_width, max_height, image);
        else error = ESP_ERR_NOT_SUPPORTED;
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
