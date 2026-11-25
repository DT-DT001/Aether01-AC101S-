#include "app_wallpaper_store.h"

#include <stddef.h>
#include <stdlib.h>
#include <string.h>
#include "esp_crc.h"
#include "esp_heap_caps.h"
#include "miniz.h"

enum {
    PARTITION_SUBTYPE = 0x40,
    SLOT_SIZE = 512 * 1024,
    WALLPAPER_MAGIC = 0x31505741U, /* AWP1 */
    WALLPAPER_VERSION = 1,
    FORMAT_EMPTY = 0,
    FORMAT_RGB565 = 1,
    FORMAT_DEFLATE = 2, /* Read legacy records only. */
    FORMAT_RGB565_RUNS = 3,
};

typedef struct {
    uint32_t magic, serial, length, payload_crc;
    uint16_t width, height;
    uint8_t format, version;
    uint16_t reserved;
    uint32_t header_crc;
} wallpaper_header_t;

_Static_assert(sizeof(wallpaper_header_t) == 28, "wallpaper header format");
_Static_assert(APP_WALLPAPER_SIZE * APP_WALLPAPER_SIZE * 2 + sizeof(wallpaper_header_t) <= SLOT_SIZE,
               "one native framebuffer must fit in a slot");

static size_t pixel_bytes(uint16_t width, uint16_t height)
{
    if (!width || !height || width > APP_WALLPAPER_SIZE || height > APP_WALLPAPER_SIZE) return 0;
    return (size_t)width * height * sizeof(uint16_t);
}

static uint32_t header_checksum(const wallpaper_header_t *header)
{
    return esp_crc32_le(0, (const uint8_t *)header, offsetof(wallpaper_header_t, header_crc));
}

static bool valid_header(const wallpaper_header_t *header)
{
    if (header->magic != WALLPAPER_MAGIC || header->version != WALLPAPER_VERSION ||
        header->reserved || header->header_crc != header_checksum(header)) return false;
    if (header->format == FORMAT_EMPTY)
        return !header->width && !header->height && !header->length && !header->payload_crc;
    const size_t raw_bytes = pixel_bytes(header->width, header->height);
    return raw_bytes && (header->format == FORMAT_RGB565 ? header->length == raw_bytes :
                         (header->format == FORMAT_DEFLATE || header->format == FORMAT_RGB565_RUNS) &&
                         header->length && header->length < raw_bytes);
}

static void *image_alloc(size_t bytes)
{
    return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
}

/* 0xxxxxxx copies 1–128 literal pixels, 10xxxxxx repeats one RGB565 pixel,
 * and 11xxxxxx copies 1–64 pixels from the preceding row. */
static bool unpack_pixels(const uint8_t *data, size_t size, uint16_t *pixels,
                          size_t count, uint16_t width)
{
    size_t pos = 0;
    while (size) {
        const uint8_t tag = *data++;
        --size;
        const size_t run = (tag & ((tag & 0x80U) ? 0x3fU : 0x7fU)) + 1U;
        if (run > count - pos) return false;
        if (tag < 0x80U) {
            const size_t bytes = run * sizeof(*pixels);
            if (size < bytes) return false;
            memcpy(pixels + pos, data, bytes);
            data += bytes;
            size -= bytes;
        } else if (tag < 0xc0U) {
            uint16_t color;
            if (size < sizeof(color)) return false;
            memcpy(&color, data, sizeof(color));
            for (size_t i = 0; i < run; ++i) pixels[pos + i] = color;
            data += sizeof(color);
            size -= sizeof(color);
        } else {
            if (pos < width) return false;
            for (size_t i = 0; i < run; ++i) pixels[pos + i] = pixels[pos + i - width];
        }
        pos += run;
    }
    return pos == count;
}

static esp_err_t load_pixels(const esp_partition_t *partition, unsigned slot,
                             const wallpaper_header_t *header, uint16_t **pixels)
{
    if (header->format == FORMAT_EMPTY) return ESP_OK;
    const size_t raw_size = pixel_bytes(header->width, header->height);
    uint8_t *raw = image_alloc(raw_size);
    if (!raw) return ESP_ERR_NO_MEM;
    const bool compressed = header->format != FORMAT_RGB565;
    uint8_t *data = compressed ? image_alloc(header->length) : raw;
    if (!data) { free(raw); return ESP_ERR_NO_MEM; }
    esp_err_t error = esp_partition_read(partition, slot * SLOT_SIZE + sizeof(*header), data, header->length);
    if (error == ESP_OK && esp_crc32_le(0, data, header->length) != header->payload_crc) error = ESP_ERR_INVALID_CRC;
    if (error == ESP_OK && header->format == FORMAT_RGB565_RUNS &&
        !unpack_pixels(data, header->length, (uint16_t *)raw, raw_size / sizeof(uint16_t), header->width))
        error = ESP_ERR_INVALID_CRC;
    if (error == ESP_OK && header->format == FORMAT_DEFLATE) {
        tinfl_decompressor *decoder = image_alloc(sizeof(*decoder));
        if (!decoder) error = ESP_ERR_NO_MEM;
        else {
            tinfl_init(decoder);
            size_t input = header->length, output = raw_size;
            if (tinfl_decompress(decoder, data, &input, raw, raw, &output,
                                 TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF) != TINFL_STATUS_DONE ||
                input != header->length || output != raw_size) error = ESP_ERR_INVALID_CRC;
            free(decoder);
        }
    }
    if (compressed) free(data);
    if (error == ESP_OK) *pixels = (uint16_t *)raw;
    else free(raw);
    return error;
}

esp_err_t app_wallpaper_store_open(app_wallpaper_store_t *store, uint16_t **pixels,
                                   uint16_t *width, uint16_t *height)
{
    *store = (app_wallpaper_store_t){.slot = 1};
    *pixels = NULL;
    *width = 0;
    *height = 0;
    const esp_partition_t *partition = esp_partition_find_first(ESP_PARTITION_TYPE_DATA,
                                                                 PARTITION_SUBTYPE, "wallpaper");
    if (!partition) return ESP_ERR_NOT_FOUND;
    if (partition->size != 2 * SLOT_SIZE || !partition->erase_size || SLOT_SIZE % partition->erase_size ||
        partition->encrypted || partition->readonly) return ESP_ERR_INVALID_SIZE;
    wallpaper_header_t headers[2];
    bool valid[2];
    for (unsigned slot = 0; slot < 2; ++slot) {
        wallpaper_header_t *header = &headers[slot];
        const esp_err_t error = esp_partition_read(partition, slot * SLOT_SIZE, header, sizeof(*header));
        if (error != ESP_OK) return error;
        if (header->magic == WALLPAPER_MAGIC && header->header_crc == header_checksum(header) &&
            header->version != WALLPAPER_VERSION) return ESP_ERR_NOT_SUPPORTED;
        valid[slot] = valid_header(header);
    }
    const uint32_t distance = headers[1].serial - headers[0].serial;
    unsigned first = valid[1] && (!valid[0] || (distance && distance <= UINT32_MAX / 2U));
    for (unsigned attempt = 0; attempt < 2; ++attempt) {
        const unsigned slot = first ^ attempt;
        const wallpaper_header_t *header = &headers[slot];
        if (!valid[slot]) continue;
        const esp_err_t error = load_pixels(partition, slot, header, pixels);
        if (error == ESP_ERR_INVALID_CRC) continue;
        if (error != ESP_OK) return error;
        store->serial = header->serial;
        store->slot = slot;
        *width = header->width;
        *height = header->height;
        break;
    }
    store->partition = partition;
    return ESP_OK;
}

static uint8_t run_tag(const uint16_t *pixels, size_t pos, size_t count, uint16_t width)
{
    const size_t limit = count - pos < 64U ? count - pos : 64U;
    if (limit >= 2U && pos >= width && pixels[pos] == pixels[pos - width]) {
        size_t run = 1;
        while (run < limit && pixels[pos + run] == pixels[pos + run - width]) ++run;
        if (run >= 2U) return 0xc0U | (run - 1U);
    }
    if (limit >= 3U && pixels[pos] == pixels[pos + 1U] && pixels[pos] == pixels[pos + 2U]) {
        size_t run = 3;
        while (run < limit && pixels[pos + run] == pixels[pos]) ++run;
        return 0x80U | (run - 1U);
    }
    return 0;
}

static size_t pack_pixels(uint8_t *output, const uint16_t *pixels, size_t count, uint16_t width)
{
    const size_t capacity = count * sizeof(*pixels) - 1U;
    size_t written = 0;
    for (size_t pos = 0; pos < count;) {
        const size_t first = pos;
        uint8_t tag = run_tag(pixels, pos, count, width);
        size_t bytes;
        if (tag) {
            pos += (tag & 0x3fU) + 1U;
            bytes = tag < 0xc0U ? sizeof(*pixels) : 0U;
        } else {
            do { ++pos; } while (pos < count && pos - first < 128U && !run_tag(pixels, pos, count, width));
            tag = pos - first - 1U;
            bytes = (pos - first) * sizeof(*pixels);
        }
        if (1U + bytes > capacity - written) return 0;
        output[written++] = tag;
        if (bytes) memcpy(output + written, pixels + first, bytes);
        written += bytes;
    }
    return written;
}

static esp_err_t verify_payload(const esp_partition_t *partition, size_t offset, size_t size, uint32_t expected)
{
    uint8_t buffer[768];
    uint32_t crc = 0;
    while (size) {
        const size_t chunk = size < sizeof(buffer) ? size : sizeof(buffer);
        const esp_err_t error = esp_partition_read(partition, offset, buffer, chunk);
        if (error != ESP_OK) return error;
        crc = esp_crc32_le(crc, buffer, chunk);
        offset += chunk;
        size -= chunk;
    }
    return crc == expected ? ESP_OK : ESP_ERR_INVALID_CRC;
}

esp_err_t app_wallpaper_store_write(app_wallpaper_store_t *store, const uint16_t *pixels,
                                    uint16_t width, uint16_t height)
{
    const esp_partition_t *partition = store->partition;
    if (!partition) return ESP_ERR_INVALID_STATE;
    const size_t bytes = pixel_bytes(width, height);
    if (pixels ? !bytes : width || height) return ESP_ERR_INVALID_ARG;
    uint8_t *compressed = bytes ? image_alloc(bytes) : NULL;
    const size_t packed = compressed ? pack_pixels(compressed, pixels, bytes / sizeof(*pixels), width) : 0;
    const uint8_t *payload = packed ? compressed : (const uint8_t *)pixels;
    wallpaper_header_t header = {
        .magic = WALLPAPER_MAGIC, .serial = store->serial + 1U, .version = WALLPAPER_VERSION,
        .width = width, .height = height, .length = packed ? packed : bytes,
        .format = packed ? FORMAT_RGB565_RUNS : pixels ? FORMAT_RGB565 : FORMAT_EMPTY,
    };
    if (header.length) header.payload_crc = esp_crc32_le(0, payload, header.length);
    header.header_crc = header_checksum(&header);

    const uint8_t slot = store->slot ^ 1U;
    const size_t offset = slot * SLOT_SIZE;
    const size_t sector = partition->erase_size;
    const size_t sectors = (sizeof(header) + header.length + sector - 1U) / sector;
    esp_err_t error = esp_partition_erase_range(partition, offset, sectors * sector);
    if (error == ESP_OK && header.length) {
        error = esp_partition_write(partition, offset + sizeof(header), payload, header.length);
        if (error == ESP_OK) error = verify_payload(partition, offset + sizeof(header), header.length, header.payload_crc);
    }
    free(compressed);
    /* The old slot stays intact. A power interruption cannot publish a torn payload. */
    if (error == ESP_OK) error = esp_partition_write(partition, offset, &header, sizeof(header));
    if (error == ESP_OK) {
        wallpaper_header_t committed;
        error = esp_partition_read(partition, offset, &committed, sizeof(committed));
        if (error == ESP_OK && memcmp(&header, &committed, sizeof(header))) error = ESP_ERR_INVALID_CRC;
    }
    if (error == ESP_OK) {
        store->slot = slot;
        store->serial = header.serial;
    }
    return error;
}
