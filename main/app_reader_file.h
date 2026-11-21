#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include "esp_err.h"

typedef enum { APP_READER_UTF8, APP_READER_GBK, APP_READER_UTF16_LE, APP_READER_UTF16_BE } app_reader_encoding_t;

typedef struct {
    char *text;
    uint64_t offset;
    uint64_t next_offset;
    uint64_t file_size;
    uint64_t text_start_offset;
    uint64_t *source_offsets; /* UTF-8 byte boundary -> original file byte offset; NULL for UTF-8. */
    app_reader_encoding_t encoding;
    uint32_t invalid_count;
} app_reader_chunk_t;

/** Read bounded UTF-8 output from UTF-8, GBK or BOM-marked UTF-16 source.
 * Call outside LVGL while holding the storage lease. The caller owns both
 * chunk->text and chunk->source_offsets and frees each with free(). On error
 * both pointers are NULL. Source positions and bookmark offsets remain bytes
 * of the original file; source_offsets has strlen(text)+1 meaningful entries.
 */
esp_err_t app_reader_file_read(const char *path, uint64_t offset, size_t limit,
                                app_reader_chunk_t *chunk);
esp_err_t app_reader_file_read_aligned(const char *path, uint64_t offset, size_t limit,
                                        bool source_boundary, app_reader_chunk_t *chunk);
/* encoding_hint: 0=auto, 1=UTF-8, 2=GBK, 3=UTF-16LE, 4=UTF-16BE. */
esp_err_t app_reader_file_read_ex(const char *path, uint64_t offset, size_t limit,
                                   bool source_boundary, unsigned encoding_hint, app_reader_chunk_t *chunk);
esp_err_t app_reader_file_read_cancelable(const char *path, uint64_t offset, size_t limit,
                                           bool source_boundary, unsigned encoding_hint,
                                           bool (*cancel)(void *), void *context, app_reader_chunk_t *chunk);
const char *app_reader_encoding_name(app_reader_encoding_t encoding);
