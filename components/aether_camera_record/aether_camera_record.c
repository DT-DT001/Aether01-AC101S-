#include "aether_camera_record.h"

#include <limits.h>
#include <string.h>
#include <unistd.h>

#include "esp_heap_caps.h"

enum {
    AVI_HEADER_BYTES = 224,
    AVI_HEADER_LIST_BYTES = 192,
    AVI_STREAM_LIST_BYTES = 116,
    AVI_CHUNK_HEADER_BYTES = 8,
    AVI_MOVI_OFFSET = AVI_HEADER_BYTES - 4,
    AVI_INDEX_ENTRY_BYTES = 16,
    AVI_INDEX_PAGE_FRAMES = 1024,
    AVI_INDEX_MAX_FRAMES = 128 * AVI_INDEX_PAGE_FRAMES,
    AVI_FILE_LIMIT = INT32_MAX,
    AVI_HAS_INDEX = 0x10,
    AVI_INDEX_KEYFRAME = 0x10,
};

typedef struct aether_camera_record_index_page {
    struct aether_camera_record_index_page *next;
    uint8_t entries[AVI_INDEX_PAGE_FRAMES][AVI_INDEX_ENTRY_BYTES];
} avi_index_page_t;

static void put_u16(uint8_t *bytes, uint16_t value)
{
    bytes[0] = (uint8_t)value;
    bytes[1] = (uint8_t)(value >> 8);
}

static void put_u32(uint8_t *bytes, uint32_t value)
{
    put_u16(bytes, (uint16_t)value);
    put_u16(bytes + 2, (uint16_t)(value >> 16));
}

static void put_chunk(uint8_t *bytes, const char tag[4], uint32_t size)
{
    memcpy(bytes, tag, 4);
    put_u32(bytes + 4, size);
}

static bool write_bytes(FILE *file, const void *buffer, size_t size)
{
    return fwrite(buffer, 1, size, file) == size;
}

static bool write_header(const aether_camera_record_t *record, uint32_t file_size,
                         uint32_t period_us, bool has_index)
{
    uint8_t header[AVI_HEADER_BYTES] = {0};
    put_chunk(header, "RIFF", file_size - AVI_CHUNK_HEADER_BYTES);
    memcpy(header + 8, "AVI ", 4);
    put_chunk(header + 12, "LIST", AVI_HEADER_LIST_BYTES);
    memcpy(header + 20, "hdrl", 4);

    put_chunk(header + 24, "avih", 56);
    uint8_t *main_header = header + 32;
    const uint64_t bytes_per_second = (uint64_t)record->largest_frame * 1000000 / period_us;
    put_u32(main_header, period_us);
    put_u32(main_header + 4, bytes_per_second > UINT32_MAX ? UINT32_MAX : (uint32_t)bytes_per_second);
    put_u32(main_header + 12, has_index ? AVI_HAS_INDEX : 0);
    put_u32(main_header + 16, record->frame_count);
    put_u32(main_header + 24, 1);  /* Video stream. */
    put_u32(main_header + 28, record->largest_frame);
    put_u32(main_header + 32, record->width);
    put_u32(main_header + 36, record->height);

    put_chunk(header + 88, "LIST", AVI_STREAM_LIST_BYTES);
    memcpy(header + 96, "strl", 4);
    put_chunk(header + 100, "strh", 56);
    uint8_t *stream_header = header + 108;
    memcpy(stream_header, "vidsMJPG", 8);
    put_u32(stream_header + 20, period_us);  /* dwScale */
    put_u32(stream_header + 24, 1000000);    /* dwRate */
    put_u32(stream_header + 32, record->frame_count);
    put_u32(stream_header + 36, record->largest_frame);
    put_u32(stream_header + 40, UINT32_MAX);  /* Default encoder quality. */
    put_u16(stream_header + 52, record->width);
    put_u16(stream_header + 54, record->height);

    put_chunk(header + 164, "strf", 40);
    uint8_t *bitmap_info = header + 172;
    put_u32(bitmap_info, 40);
    put_u32(bitmap_info + 4, record->width);
    put_u32(bitmap_info + 8, record->height);
    put_u16(bitmap_info + 12, 1);  /* biPlanes */
    put_u16(bitmap_info + 14, 24);
    memcpy(bitmap_info + 16, "MJPG", 4);
    put_u32(bitmap_info + 20, (uint32_t)record->width * record->height * 3);

    put_chunk(header + 212, "LIST", record->file_offset - AVI_MOVI_OFFSET);
    memcpy(header + AVI_MOVI_OFFSET, "movi", 4);
    return write_bytes(record->file, header, sizeof(header));
}

esp_err_t aether_camera_record_start(aether_camera_record_t *record, FILE *file,
                                    uint16_t width, uint16_t height,
                                    uint32_t frame_interval_100ns)
{
    if (!record || !file || width == 0 || width > INT16_MAX || height == 0 ||
        height > INT16_MAX || frame_interval_100ns < 10) {
        return ESP_ERR_INVALID_ARG;
    }
    if (record->file || ftell(file) != 0) {
        return ESP_ERR_INVALID_STATE;
    }

    *record = (aether_camera_record_t) {
        .file = file,
        .file_offset = AVI_HEADER_BYTES,
        .nominal_period_us = frame_interval_100ns / 10,
        .width = width,
        .height = height,
    };
    if (!write_header(record, AVI_HEADER_BYTES, record->nominal_period_us, false)) {
        aether_camera_record_abort(record);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t aether_camera_record_write(aether_camera_record_t *record, const void *jpeg, size_t size)
{
    if (!record || !jpeg || size < 4) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!record->file || record->short_write) {
        return ESP_ERR_INVALID_STATE;
    }
    const uint8_t *frame = jpeg;
    if (frame[0] != 0xff || frame[1] != 0xd8) {
        return ESP_ERR_INVALID_RESPONSE;
    }
    if (size > AVI_FILE_LIMIT || record->frame_count == AVI_INDEX_MAX_FRAMES) {
        return ESP_ERR_INVALID_SIZE;
    }
    const uint8_t padding = (uint8_t)(size & 1);
    const uint32_t frame_size = (uint32_t)size + AVI_CHUNK_HEADER_BYTES + padding;
    const uint32_t index_size = (record->frame_count + 1) * AVI_INDEX_ENTRY_BYTES + AVI_CHUNK_HEADER_BYTES;
    if ((uint64_t)record->file_offset + frame_size + index_size > AVI_FILE_LIMIT) {
        return ESP_ERR_INVALID_SIZE;
    }

    const uint32_t position = record->frame_count % AVI_INDEX_PAGE_FRAMES;
    if (position == 0) {
        avi_index_page_t *page = heap_caps_malloc(sizeof(*page), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (!page) {
            return ESP_ERR_NO_MEM;
        }
        page->next = NULL;
        if (record->index_tail) {
            record->index_tail->next = page;
        } else {
            record->index_head = page;
        }
        record->index_tail = page;
    }

    uint8_t chunk[AVI_CHUNK_HEADER_BYTES];
    const uint8_t pad = 0;
    put_chunk(chunk, "00dc", (uint32_t)size);
    if (!write_bytes(record->file, chunk, sizeof(chunk)) || !write_bytes(record->file, jpeg, size) ||
        (padding && !write_bytes(record->file, &pad, padding))) {
        record->short_write = true;
        return ESP_FAIL;
    }

    uint8_t *entry = record->index_tail->entries[position];
    memcpy(entry, "00dc", 4);
    put_u32(entry + 4, AVI_INDEX_KEYFRAME);
    put_u32(entry + 8, record->file_offset - AVI_MOVI_OFFSET);
    put_u32(entry + 12, (uint32_t)size);
    record->file_offset += frame_size;
    record->frame_count++;
    if (record->largest_frame < size) {
        record->largest_frame = (uint32_t)size;
    }
    return ESP_OK;
}

static bool write_index(const aether_camera_record_t *record)
{
    uint8_t chunk[AVI_CHUNK_HEADER_BYTES];
    put_chunk(chunk, "idx1", record->frame_count * AVI_INDEX_ENTRY_BYTES);
    if (!write_bytes(record->file, chunk, sizeof(chunk))) {
        return false;
    }
    const avi_index_page_t *page = record->index_head;
    uint32_t remaining = record->frame_count;
    while (remaining > 0) {
        const uint32_t count = remaining > AVI_INDEX_PAGE_FRAMES ? AVI_INDEX_PAGE_FRAMES : remaining;
        if (!write_bytes(record->file, page->entries, count * AVI_INDEX_ENTRY_BYTES)) {
            return false;
        }
        page = page->next;
        remaining -= count;
    }
    return true;
}

static esp_err_t finalize_file(const aether_camera_record_t *record, uint64_t duration_us)
{
    if (record->frame_count == 0) {
        return ESP_ERR_NOT_FOUND;
    }
    FILE *file = record->file;
    if (record->short_write) {
        /* Cut off the incomplete frame before attempting to write the index. */
        clearerr(file);
        if (fflush(file) != 0 || ftruncate(fileno(file), (off_t)record->file_offset) != 0 ||
            fseek(file, (long)record->file_offset, SEEK_SET) != 0) {
            return ESP_FAIL;
        }
    }

    const uint64_t measured_period = duration_us / record->frame_count;
    const uint32_t period = measured_period == 0 || measured_period > UINT32_MAX ?
                            record->nominal_period_us : (uint32_t)measured_period;
    const uint32_t file_size = record->file_offset + AVI_CHUNK_HEADER_BYTES +
                               record->frame_count * AVI_INDEX_ENTRY_BYTES;
    if (!write_index(record) || fseek(file, 0, SEEK_SET) != 0 ||
        !write_header(record, file_size, period, true) || fflush(file) != 0) {
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t aether_camera_record_finish(aether_camera_record_t *record, uint64_t capture_duration_us)
{
    if (!record || !record->file) {
        return ESP_ERR_INVALID_STATE;
    }
    const esp_err_t result = finalize_file(record, capture_duration_us);
    aether_camera_record_abort(record);
    return result;
}

void aether_camera_record_abort(aether_camera_record_t *record)
{
    if (!record) {
        return;
    }
    avi_index_page_t *page = record->index_head;
    while (page) {
        avi_index_page_t *next = page->next;
        heap_caps_free(page);
        page = next;
    }
    *record = (aether_camera_record_t) {0};
}
