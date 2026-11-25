#include "app_wallpaper_store.h"
#include "esp_crc.h"
#include "miniz.h"
#include "nvs.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { SECTOR = 4096, SLOT = 512 * 1024, HEADER = 28 };
enum fault { NO_FAULT, FAILED_ERASE, TORN_PAYLOAD, BAD_VERIFY, TORN_HEADER };
static uint8_t flash[2 * SLOT];
static bool has_partition = true;
static enum fault fault;
static int palette_saved = -1, palette_staged;
static const esp_partition_t part = {.size = sizeof(flash), .erase_size = SECTOR};

const esp_partition_t *esp_partition_find_first(int type, int subtype, const char *label)
{
    assert(type == ESP_PARTITION_TYPE_DATA && subtype == 0x40 && !strcmp(label, "wallpaper"));
    return has_partition ? &part : NULL;
}

esp_err_t esp_partition_read(const esp_partition_t *partition, size_t offset, void *output, size_t length)
{
    assert(partition == &part && offset <= sizeof(flash) && length <= sizeof(flash) - offset);
    memcpy(output, flash + offset, length);
    if (fault == BAD_VERIFY && offset % SLOT == HEADER) {
        fault = NO_FAULT;
        ((uint8_t *)output)[0] ^= 1;
    }
    return ESP_OK;
}

esp_err_t esp_partition_write(const esp_partition_t *partition, size_t offset, const void *input, size_t length)
{
    assert(partition == &part && offset <= sizeof(flash) && length <= sizeof(flash) - offset);
    const bool truncated = (fault == TORN_PAYLOAD && offset % SLOT == HEADER) ||
                           (fault == TORN_HEADER && offset % SLOT == 0);
    const size_t written = truncated ? length / 2 : length;
    for (size_t i = 0; i < written; ++i) {
        const uint8_t byte = ((const uint8_t *)input)[i];
        assert((flash[offset + i] & byte) == byte);
        flash[offset + i] = byte;
    }
    if (truncated) fault = NO_FAULT;
    return truncated ? ESP_FAIL : ESP_OK;
}

esp_err_t esp_partition_erase_range(const esp_partition_t *partition, size_t offset, size_t length)
{
    assert(partition == &part && offset % SLOT == 0 && length <= SLOT && length % SECTOR == 0);
    if (fault == FAILED_ERASE) { fault = NO_FAULT; return ESP_FAIL; }
    memset(flash + offset, 255, length);
    return ESP_OK;
}

uint32_t esp_crc32_le(uint32_t crc, const uint8_t *bytes, uint32_t length)
{
    crc = ~crc;
    while (length--) {
        crc ^= *bytes++;
        for (unsigned bit = 0; bit < 8; ++bit) crc = (crc >> 1) ^ (0xEDB88320U & (0U - (crc & 1U)));
    }
    return ~crc;
}

int tinfl_decompress(tinfl_decompressor *decoder, const uint8_t *input, size_t *bytes,
                     uint8_t *output, uint8_t *next, size_t *available, unsigned flags)
{
    assert(decoder->state == 0 && flags == TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF && next == output);
    if (*bytes != 2 || *available % 2) return -1;
    for (size_t i = 0; i < *available; ++i) output[i] = input[i % 2];
    return TINFL_STATUS_DONE;
}

esp_err_t nvs_open(const char *space, int mode, nvs_handle_t *handle)
{
    assert(!strcmp(space, "appearance"));
    if (mode == NVS_READONLY && palette_saved < 0) return ESP_ERR_NOT_FOUND;
    *handle = mode;
    return ESP_OK;
}

esp_err_t nvs_get_u8(nvs_handle_t handle, const char *key, uint8_t *value)
{
    assert(handle == NVS_READONLY && !strcmp(key, "palette"));
    *value = palette_saved;
    return ESP_OK;
}

esp_err_t nvs_set_u8(nvs_handle_t handle, const char *key, uint8_t value)
{
    assert(handle == NVS_READWRITE && !strcmp(key, "palette"));
    palette_staged = value;
    return ESP_OK;
}

esp_err_t nvs_commit(nvs_handle_t handle)
{
    assert(handle == NVS_READWRITE);
    palette_saved = palette_staged;
    return ESP_OK;
}

void nvs_close(nvs_handle_t handle) { assert(handle == NVS_READONLY || handle == NVS_READWRITE); }

static void verify_pixels(const uint16_t *expected, uint16_t width, uint16_t height)
{
    app_wallpaper_store_t rebooted;
    uint16_t *pixels, w, h;
    assert(app_wallpaper_store_open(&rebooted, &pixels, &w, &h) == ESP_OK && w == width && h == height);
    if (expected) assert(pixels && !memcmp(pixels, expected, (size_t)w * h * sizeof(*pixels)));
    else assert(!pixels);
    free(pixels);
}

static void fixture_payload(unsigned slot, uint8_t format, const void *payload, uint32_t length)
{
    uint8_t *record = flash + slot * SLOT;
    record[20] = format;
    memcpy(record + HEADER, payload, length);
    memcpy(record + 8, &length, sizeof(length));
    uint32_t crc = esp_crc32_le(0, record + HEADER, length);
    memcpy(record + 12, &crc, sizeof(crc));
    crc = esp_crc32_le(0, record, HEADER - sizeof(crc));
    memcpy(record + HEADER - sizeof(crc), &crc, sizeof(crc));
}

void verify_wallpaper_faults(void)
{
    app_wallpaper_store_t store;
    uint16_t *pixels, w, h;
    has_partition = false;
    assert(app_wallpaper_store_open(&store, &pixels, &w, &h) == ESP_ERR_NOT_FOUND);
    assert(app_wallpaper_store_write(&store, NULL, 0, 0) == ESP_ERR_INVALID_STATE);
    has_partition = true;
    memset(flash, 255, sizeof(flash));
    assert(app_wallpaper_store_open(&store, &pixels, &w, &h) == ESP_OK && !pixels && !w && !h);
    assert(app_wallpaper_store_write(&store, NULL, 480, 480) == ESP_ERR_INVALID_ARG);
    static uint16_t raw[480 * 480];
    static uint16_t packed[480 * 320];
    for (size_t i = 0; i < sizeof(raw) / sizeof(raw[0]); ++i) raw[i] = (uint16_t)(i * 3977U);
    for (size_t i = 0; i < sizeof(packed) / sizeof(packed[0]); ++i) packed[i] = 0xF81F;
    assert(app_wallpaper_store_write(&store, raw, 481, 480) == ESP_ERR_INVALID_ARG);
    assert(app_wallpaper_store_write(&store, raw, 480, 480) == ESP_OK && flash[20] == 1);
    verify_pixels(raw, 480, 480);
    for (enum fault kind = FAILED_ERASE; kind <= TORN_HEADER; ++kind) {
        fault = kind;
        assert(app_wallpaper_store_write(&store, packed, 480, 320) != ESP_OK && fault == NO_FAULT);
        verify_pixels(raw, 480, 480);
    }
    assert(app_wallpaper_store_write(&store, packed, 480, 320) == ESP_OK && flash[SLOT + 20] == 3);
    verify_pixels(packed, 480, 320);
    fixture_payload(1, 2, packed, sizeof(packed[0]));
    verify_pixels(packed, 480, 320);
    flash[SLOT + HEADER] ^= 0x80;
    verify_pixels(raw, 480, 480);
    assert(app_wallpaper_store_write(&store, NULL, 0, 0) == ESP_OK);
    verify_pixels(NULL, 0, 0);

    uint16_t mixed[133 * 9];
    for (unsigned y = 0; y < 9; ++y)
        for (unsigned x = 0; x < 133; ++x)
            mixed[y * 133 + x] = x < 129 ? x * 17U : 0x7c00U;
    assert(app_wallpaper_store_write(&store, mixed, 133, 9) == ESP_OK && flash[SLOT + 20] == 3);
    assert(flash[SLOT + HEADER] == 127 && flash[SLOT + HEADER + 257] == 0);
    verify_pixels(mixed, 133, 9);
    const uint8_t invalid[] = {0xc0}; /* A first-row back-reference has no source. */
    fixture_payload(1, 3, invalid, sizeof(invalid));
    verify_pixels(NULL, 0, 0);
    puts("PASS wallpaper Flash: raw/RLE/legacy reload, run boundaries, malformed stream, failed writes, corruption fallback, durable clear");
}
