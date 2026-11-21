#include "mjpeg_stream.h"
#include <string.h>

static esp_err_t read_byte(aether_mjpeg_reader_t *reader, uint8_t *value)
{
    if (reader->read_limit && reader->offset >= reader->read_limit) return ESP_ERR_INVALID_SIZE;
    if (reader->position == reader->length) {
        if (reader->cancelled && reader->cancelled(reader->context)) return ESP_ERR_INVALID_STATE;
        reader->length = fread(reader->buffer, 1, AETHER_MJPEG_READ_BYTES, reader->file);
        reader->position = 0;
        if (!reader->length) return ferror(reader->file) ? ESP_FAIL : ESP_ERR_NOT_FOUND;
        if ((++reader->refill_count & 31U) == 0 && reader->yield) reader->yield(reader->context);
    }
    *value = reader->buffer[reader->position++];
    ++reader->offset;
    return ESP_OK;
}

static esp_err_t read_bytes(aether_mjpeg_reader_t *reader, uint8_t *data, size_t size)
{
    for (size_t i = 0; i < size; ++i) {
        esp_err_t err = read_byte(reader, data + i);
        if (err != ESP_OK) return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_SIZE : err;
    }
    return ESP_OK;
}

static esp_err_t skip_to(aether_mjpeg_reader_t *reader, uint64_t end)
{
    if (reader->offset > end) return ESP_ERR_INVALID_SIZE;
    while (reader->offset < end) {
        if (reader->cancelled && reader->cancelled(reader->context)) return ESP_ERR_INVALID_STATE;
        if (reader->position == reader->length) {
            uint8_t unused;
            esp_err_t err = read_byte(reader, &unused);
            if (err != ESP_OK) return err == ESP_ERR_NOT_FOUND ? ESP_ERR_INVALID_SIZE : err;
        } else {
            uint64_t left = end - reader->offset;
            size_t available = reader->length - reader->position;
            size_t take = left < available ? (size_t)left : available;
            reader->position += take;
            reader->offset += take;
        }
    }
    return ESP_OK;
}

static uint32_t le32(const uint8_t *bytes)
{
    return (uint32_t)bytes[0] | ((uint32_t)bytes[1] << 8) |
           ((uint32_t)bytes[2] << 16) | ((uint32_t)bytes[3] << 24);
}

static esp_err_t initialize(aether_mjpeg_reader_t *reader)
{
    /* Probe through the same buffer, then rewind its cursor for raw MJPEG.
     * Do not scan RIFF headers/audio/thumbnail payloads for JPEG markers. */
    uint8_t header[12];
    esp_err_t err = read_byte(reader, header);
    if (err != ESP_OK) return err;
    /* A first refill already contains up to 4096 bytes. Peeking directly
     * avoids replacing a short raw-JPEG buffer with the next EOF refill. */
    size_t count = reader->length < sizeof(header) ? reader->length : sizeof(header);
    memcpy(header, reader->buffer, count);
    reader->position = count;
    reader->offset = count;
    reader->initialized = true;
    if (count >= 4 && !memcmp(header, "RIFF", 4)) {
        if (count != sizeof(header)) return ESP_ERR_INVALID_SIZE;
        if (memcmp(header + 8, "AVI ", 4)) return ESP_ERR_NOT_SUPPORTED;
        uint32_t size = le32(header + 4);
        if (size < 4) return ESP_ERR_INVALID_SIZE;
        const long buffered_end = ftell(reader->file);
        if (buffered_end < 0 || fseek(reader->file, 0, SEEK_END)) return ESP_FAIL;
        const long file_size = ftell(reader->file);
        if (fseek(reader->file, buffered_end, SEEK_SET)) return ESP_FAIL;
        if (file_size < 0) return ESP_ERR_NOT_SUPPORTED;
        if ((uint64_t)file_size < (uint64_t)size + 8) return ESP_ERR_INVALID_SIZE;
        /* OpenDML/AVIX multi-RIFF continuation requires a stream index and is
         * not this classic-AVI implementation. Never silently play only part. */
        if ((uint64_t)file_size > (uint64_t)size + 8) return ESP_ERR_NOT_SUPPORTED;
        reader->avi = true;
        reader->depth = 1;
        reader->list_end[0] = (uint64_t)size + 8;
        reader->list_padded_end[0] = reader->list_end[0];
        reader->list_movi[0] = false;
    } else {
        reader->position = 0;
        reader->offset = 0;
    }
    return ESP_OK;
}

static esp_err_t append_byte(aether_mjpeg_reader_t *reader, uint8_t *frame,
                             size_t capacity, size_t *used, uint8_t *value)
{
    if (*used >= capacity) return ESP_ERR_INVALID_SIZE;
    esp_err_t result = read_byte(reader, value);
    if (result == ESP_ERR_NOT_FOUND) return ESP_ERR_INVALID_SIZE;
    if (result != ESP_OK) return result;
    frame[(*used)++] = *value;
    return ESP_OK;
}

static esp_err_t read_jpeg(aether_mjpeg_reader_t *reader, uint8_t *frame,
                           size_t capacity, size_t *frame_size, bool scan_start)
{
    uint8_t previous = 0, value = 0;
    esp_err_t result;
    if (scan_start) {
        do {
            result = read_byte(reader, &value);
            if (result != ESP_OK) return result;
            if (previous == 0xFF && value == 0xD8) break;
            previous = value;
        } while (true);
    } else {
        uint8_t signature[2];
        result = read_bytes(reader, signature, sizeof(signature));
        if (result != ESP_OK) return result;
        if (signature[0] != 0xFF || signature[1] != 0xD8) return ESP_ERR_INVALID_RESPONSE;
    }
    frame[0] = 0xFF;
    frame[1] = 0xD8;
    size_t used = 2;
    bool entropy = false;

    for (;;) {
        result = append_byte(reader, frame, capacity, &used, &value);
        if (result != ESP_OK) return result;
        if (value != 0xFF) {
            if (entropy) continue;
            return ESP_ERR_INVALID_RESPONSE;
        }
        do {
            result = append_byte(reader, frame, capacity, &used, &value);
            if (result != ESP_OK) return result;
        } while (value == 0xFF);
        if (entropy && (value == 0x00 || (value >= 0xD0 && value <= 0xD7))) continue;
        if (value == 0xD9) {
            *frame_size = used;
            return ESP_OK;
        }
        if (value == 0xD8 || value == 0x00) return ESP_ERR_INVALID_RESPONSE;
        if (value >= 0xC0 && value <= 0xCF && value != 0xC0 &&
            value != 0xC4 && value != 0xC8 && value != 0xCC)
            return ESP_ERR_NOT_SUPPORTED; /* The S31 hardware path is baseline JPEG only. */
        if (value == 0x01 || (value >= 0xD0 && value <= 0xD7)) continue;
        const bool scan = value == 0xDA;
        uint8_t high, low;
        result = append_byte(reader, frame, capacity, &used, &high);
        if (result != ESP_OK) return result;
        result = append_byte(reader, frame, capacity, &used, &low);
        if (result != ESP_OK) return result;
        size_t segment_size = ((size_t)high << 8) | low;
        if (segment_size < 2 || segment_size - 2 > capacity - used) return ESP_ERR_INVALID_SIZE;
        for (size_t i = 2; i < segment_size; ++i) {
            result = append_byte(reader, frame, capacity, &used, &value);
            if (result != ESP_OK) return result;
        }
        entropy = scan;
    }
}

static esp_err_t read_avi_frame(aether_mjpeg_reader_t *reader, uint8_t *frame,
                                size_t capacity, size_t *frame_size)
{
    for (;;) {
        if (!reader->depth) return ESP_ERR_NOT_FOUND;
        unsigned level = reader->depth - 1U;
        uint64_t parent_end = reader->list_end[level];
        if (reader->offset == parent_end) {
            esp_err_t err = skip_to(reader, reader->list_padded_end[level]);
            if (err != ESP_OK) return err;
            --reader->depth;
            continue;
        }
        if (reader->offset > parent_end || parent_end - reader->offset < 8) return ESP_ERR_INVALID_SIZE;
        uint8_t chunk[8];
        esp_err_t err = read_bytes(reader, chunk, sizeof(chunk));
        if (err != ESP_OK) return err;
        uint32_t size = le32(chunk + 4);
        uint64_t end = reader->offset + size;
        uint64_t padded_end = end + (size & 1U);
        if (end < reader->offset || padded_end > parent_end) return ESP_ERR_INVALID_SIZE;
        if (!memcmp(chunk, "LIST", 4)) {
            if (size < 4 || reader->depth >= 8) return ESP_ERR_INVALID_SIZE;
            uint8_t kind[4];
            err = read_bytes(reader, kind, sizeof(kind));
            if (err != ESP_OK) return err;
            unsigned child = reader->depth++;
            reader->list_end[child] = end;
            reader->list_padded_end[child] = padded_end;
            reader->list_movi[child] = reader->list_movi[level] || !memcmp(kind, "movi", 4);
            continue;
        }
        if (!reader->list_movi[level] && !memcmp(chunk, "avih", 4) && size >= 4) {
            uint8_t period[4];
            err = read_bytes(reader, period, sizeof(period));
            if (err != ESP_OK) return err;
            reader->frame_period_us = le32(period);
        }
        bool video = reader->list_movi[level] &&
            chunk[0] >= '0' && chunk[0] <= '9' && chunk[1] >= '0' && chunk[1] <= '9' &&
            chunk[2] == 'd' && (chunk[3] == 'c' || chunk[3] == 'b');
        if (video) {
            const uint16_t stream = ((uint16_t)chunk[0] << 8) | chunk[1];
            if (!reader->video_stream_selected) {
                reader->video_stream_selected = true;
                reader->video_stream_id = stream;
            }
            video = stream == reader->video_stream_id;
        }
        if (video) {
            if (size < 2 || size > capacity) return ESP_ERR_INVALID_SIZE;
            reader->read_limit = end;
            err = read_jpeg(reader, frame, capacity, frame_size, false);
            reader->read_limit = 0;
            if (err != ESP_OK) return err;
            err = skip_to(reader, padded_end);
            return err;
        }
        err = skip_to(reader, padded_end);
        if (err != ESP_OK) return err;
    }
}

esp_err_t aether_mjpeg_read_frame(aether_mjpeg_reader_t *reader, uint8_t *frame,
                                  size_t capacity, size_t *frame_size)
{
    if (!reader || !reader->file || !reader->buffer || !frame || !frame_size || capacity < 2)
        return ESP_ERR_INVALID_ARG;
    *frame_size = 0;
    if (reader->cancelled && reader->cancelled(reader->context)) return ESP_ERR_INVALID_STATE;
    if (!reader->initialized) {
        esp_err_t err = initialize(reader);
        if (err != ESP_OK) return err;
    }
    return reader->avi ? read_avi_frame(reader, frame, capacity, frame_size) :
                         read_jpeg(reader, frame, capacity, frame_size, true);
}
