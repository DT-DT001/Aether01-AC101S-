#include "app_reader_file.h"
#include "app_text.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "esp_heap_caps.h"
#include "ff.h"

#define READER_FILE_MAX_CHUNK 32768U

const char *app_reader_encoding_name(app_reader_encoding_t encoding)
{
    static const char *const names[] = {"UTF-8", "GBK", "UTF-16LE", "UTF-16BE"};
    return (unsigned)encoding <= APP_READER_UTF16_BE ? names[encoding] : "?";
}

static unsigned utf8_errors(const unsigned char *s, size_t length, bool final)
{
    unsigned errors = 0;
    for (size_t i = 0; i < length;) {
        unsigned n = s[i] < 0x80 ? 1 : s[i] >= 0xC2 && s[i] <= 0xDF ? 2 :
                     s[i] >= 0xE0 && s[i] <= 0xEF ? 3 : s[i] >= 0xF0 && s[i] <= 0xF4 ? 4 : 0;
        if (!n) { ++errors; ++i; continue; }
        if (i + n > length) { if (final) errors += (unsigned)(length - i); break; }
        bool valid = true;
        for (unsigned j = 1; j < n; ++j) if ((s[i+j] & 0xC0) != 0x80) valid = false;
        if (n > 1 && ((s[i] == 0xE0 && s[i+1] < 0xA0) || (s[i] == 0xED && s[i+1] >= 0xA0) ||
                     (s[i] == 0xF0 && s[i+1] < 0x90) || (s[i] == 0xF4 && s[i+1] >= 0x90))) valid = false;
        if (valid) i += n; else { ++errors; ++i; }
    }
    return errors;
}

static unsigned encode_utf8(uint32_t cp, char out[4])
{
    if (cp < 0x80) { out[0] = (char)cp; return 1; }
    if (cp < 0x800) { out[0] = (char)(0xC0 | (cp >> 6)); out[1] = (char)(0x80 | (cp & 63)); return 2; }
    if (cp < 0x10000) { out[0] = (char)(0xE0 | (cp >> 12)); out[1] = (char)(0x80 | ((cp >> 6) & 63)); out[2] = (char)(0x80 | (cp & 63)); return 3; }
    out[0] = (char)(0xF0 | (cp >> 18)); out[1] = (char)(0x80 | ((cp >> 12) & 63));
    out[2] = (char)(0x80 | ((cp >> 6) & 63)); out[3] = (char)(0x80 | (cp & 63)); return 4;
}

static bool gbk_pair(int lead, int trail)
{
    return lead >= 0x81 && lead <= 0xFE && trail >= 0x40 && trail <= 0xFE && trail != 0x7F;
}

/* A seek can land on a GBK trail byte, whose value can also be a lead byte.
 * Start at a guaranteed boundary (ASCII below 40h/newline or BOF), then walk
 * forward. Normal next/previous/reflow calls already carry source boundaries. */
static esp_err_t align_gbk(FILE *file, uint64_t wanted, uint64_t *aligned,
                            bool (*cancel)(void *), void *context)
{
    uint64_t anchor = wanted;
    unsigned char block[256];
    bool found = false;
    while (anchor && !found) {
        if (cancel && cancel(context)) return ESP_ERR_INVALID_STATE;
        const size_t n = anchor < sizeof(block) ? (size_t)anchor : sizeof(block);
        anchor -= n;
        if (fseek(file, (long)anchor, SEEK_SET) || fread(block, 1, n, file) != n) return ESP_FAIL;
        for (size_t i = n; i; --i) if (block[i-1] < 0x40 || block[i-1] == 0x7F || block[i-1] == 0xFF) {
            anchor += i; found = true; break;
        }
    }
    if (fseek(file, (long)anchor, SEEK_SET)) return ESP_FAIL;
    unsigned steps = 0;
    while (anchor < wanted) {
        if ((++steps & 127U) == 0 && cancel && cancel(context)) return ESP_ERR_INVALID_STATE;
        int first = fgetc(file); if (first == EOF) break; ++anchor;
        if (first >= 0x81 && first <= 0xFE) {
            int next = fgetc(file);
            if (gbk_pair(first, next)) ++anchor;
            else if (next != EOF) ungetc(next, file);
        }
    }
    *aligned = anchor;
    return ferror(file) ? ESP_FAIL : ESP_OK;
}

static int read_unit(FILE *file, bool little, uint64_t *cursor)
{
    int a = fgetc(file); if (a == EOF) return -1; ++*cursor;
    int b = fgetc(file); if (b == EOF) return -2; ++*cursor;
    return little ? a | (b << 8) : (a << 8) | b;
}

static esp_err_t read_converted(FILE *file, const char *path, uint64_t offset, size_t limit,
                                  bool boundary, bool (*cancel)(void *), void *context, app_reader_chunk_t *chunk)
{
    (void)path;
    if (offset < chunk->text_start_offset) offset = chunk->text_start_offset;
    if (chunk->encoding == APP_READER_GBK) {
        if (!boundary) {
            esp_err_t error = align_gbk(file, offset, &offset, cancel, context); if (error != ESP_OK) return error;
        }
    } else {
        if ((offset - chunk->text_start_offset) & 1U) ++offset;
        if (offset > chunk->file_size) offset = chunk->file_size;
        if (offset >= chunk->text_start_offset + 2 && offset < chunk->file_size) {
            if (fseek(file, (long)(offset - 2), SEEK_SET)) return ESP_FAIL;
            uint64_t probe = offset - 2;
            int before = read_unit(file, chunk->encoding == APP_READER_UTF16_LE, &probe);
            int here = read_unit(file, chunk->encoding == APP_READER_UTF16_LE, &probe);
            if (before >= 0xD800 && before <= 0xDBFF && here >= 0xDC00 && here <= 0xDFFF) offset += 2;
        }
    }
    if (fseek(file, (long)offset, SEEK_SET)) return ESP_FAIL;
    chunk->source_offsets = heap_caps_calloc(limit + 1U, sizeof(*chunk->source_offsets), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk->source_offsets) return ESP_ERR_NO_MEM;
    chunk->offset = offset; chunk->source_offsets[0] = offset;
    uint64_t cursor = offset;
    size_t written = 0;
    unsigned steps = 0;
    while (written < limit) {
        if ((++steps & 127U) == 0 && cancel && cancel(context)) return ESP_ERR_INVALID_STATE;
        const uint64_t start = cursor;
        uint32_t cp;
        if (chunk->encoding == APP_READER_GBK) {
            int first = fgetc(file); if (first == EOF) break; ++cursor;
            cp = (uint32_t)first;
            if (first >= 0x80) {
                int next = fgetc(file);
                if (gbk_pair(first, next)) { ++cursor; cp = ff_oem2uni((WCHAR)((first << 8) | next), 936); }
                else { if (next != EOF) ungetc(next, file); cp = 0; }
            }
        } else {
            int unit = read_unit(file, chunk->encoding == APP_READER_UTF16_LE, &cursor);
            if (unit == -1) break;
            cp = unit < 0 ? 0 : (uint32_t)unit;
            if (unit >= 0xD800 && unit <= 0xDBFF) {
                int low = read_unit(file, chunk->encoding == APP_READER_UTF16_LE, &cursor);
                if (low >= 0xDC00 && low <= 0xDFFF) cp = 0x10000U + (((uint32_t)unit - 0xD800U) << 10) + (uint32_t)low - 0xDC00U;
                else {
                    if (low >= 0) { if (fseek(file, -2, SEEK_CUR)) return ESP_FAIL; cursor -= 2; }
                    cp = 0;
                }
            } else if (unit >= 0xDC00 && unit <= 0xDFFF) cp = 0;
        }
        const bool invalid = !cp;
        if (invalid) cp = '?';
        char encoded[4]; const unsigned n = encode_utf8(cp, encoded);
        if (written + n > limit) { cursor = start; break; }
        for (unsigned i = 0; i < n; ++i) chunk->source_offsets[written + i] = start;
        memcpy(chunk->text + written, encoded, n); written += n;
        chunk->source_offsets[written] = cursor;
        if (invalid) ++chunk->invalid_count;
    }
    chunk->text[written] = 0; chunk->next_offset = cursor;
    return ferror(file) ? ESP_FAIL : ESP_OK;
}

esp_err_t app_reader_file_read(const char *path, uint64_t offset, size_t limit,
                                app_reader_chunk_t *chunk)
{
    return app_reader_file_read_aligned(path, offset, limit, false, chunk);
}

esp_err_t app_reader_file_read_aligned(const char *path, uint64_t offset, size_t limit,
                                        bool source_boundary, app_reader_chunk_t *chunk)
{
    return app_reader_file_read_ex(path, offset, limit, source_boundary, 0, chunk);
}

esp_err_t app_reader_file_read_ex(const char *path, uint64_t offset, size_t limit,
                                   bool source_boundary, unsigned encoding_hint, app_reader_chunk_t *chunk)
{
    return app_reader_file_read_cancelable(path, offset, limit, source_boundary, encoding_hint, NULL, NULL, chunk);
}

esp_err_t app_reader_file_read_cancelable(const char *path, uint64_t offset, size_t limit,
                                           bool source_boundary, unsigned encoding_hint,
                                           bool (*cancel)(void *), void *context, app_reader_chunk_t *chunk)
{
    if (!chunk) return ESP_ERR_INVALID_ARG;
    memset(chunk, 0, sizeof(*chunk));
    if (!path || encoding_hint > 4) return ESP_ERR_INVALID_ARG;
    if (cancel && cancel(context)) return ESP_ERR_INVALID_STATE;
    FILE *file = fopen(path, "rb");
    if (!file) return ESP_ERR_NOT_FOUND;
    esp_err_t error = ESP_OK;
    unsigned char header[512];
    size_t header_size = fread(header, 1, sizeof(header), file);
    if (ferror(file)) { error = ESP_FAIL; goto done; }
    if (header_size >= 4 && (!memcmp(header, "\xFF\xFE\0\0", 4) || !memcmp(header, "\0\0\xFE\xFF", 4))) {
        error = ESP_ERR_NOT_SUPPORTED;
        goto done;
    }
    chunk->text_start_offset = header_size >= 3 &&
        !memcmp(header, "\xEF\xBB\xBF", 3) ? 3U : 0U;
    if (fseek(file, 0, SEEK_END)) { error = ESP_FAIL; goto done; }
    const long size = ftell(file);
    if (size < 0) { error = ESP_FAIL; goto done; }
    chunk->file_size = (uint64_t)size;
    if (header_size >= 2 && ((header[0] == 0xFF && header[1] == 0xFE) ||
                             (header[0] == 0xFE && header[1] == 0xFF))) {
        chunk->encoding = header[0] == 0xFF ? APP_READER_UTF16_LE : APP_READER_UTF16_BE;
        chunk->text_start_offset = 2;
    } else if (!chunk->text_start_offset && utf8_errors(header, header_size, false)) chunk->encoding = APP_READER_GBK;
    if (encoding_hint) {
        chunk->encoding = (app_reader_encoding_t)(encoding_hint - 1U);
        chunk->text_start_offset = chunk->encoding == APP_READER_UTF8 && header_size >= 3 && !memcmp(header,"\xEF\xBB\xBF",3) ? 3U :
            chunk->encoding == APP_READER_UTF16_LE && header_size >= 2 && header[0] == 0xFF && header[1] == 0xFE ? 2U :
            chunk->encoding == APP_READER_UTF16_BE && header_size >= 2 && header[0] == 0xFE && header[1] == 0xFF ? 2U : 0U;
    }
    chunk->offset = offset > chunk->file_size ? chunk->file_size : offset;
    if (fseek(file, (long)chunk->offset, SEEK_SET)) { error = ESP_FAIL; goto done; }
    if (!limit) limit = 8192;
    if (limit < 8) limit = 8; /* Enough for BOM plus a full supplementary codepoint. */
    if (limit > READER_FILE_MAX_CHUNK) limit = READER_FILE_MAX_CHUNK;
    chunk->text = heap_caps_calloc(1, limit + 1U, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    if (!chunk->text) { error = ESP_ERR_NO_MEM; goto done; }
    if (chunk->encoding != APP_READER_UTF8) {
        error = read_converted(file, path, chunk->offset, limit, source_boundary, cancel, context, chunk); goto done;
    }
    size_t length = fread(chunk->text, 1, limit, file);
    if (ferror(file)) { error = ESP_FAIL; goto done; }
    if (memchr(chunk->text, 0, length)) { error = ESP_ERR_NOT_SUPPORTED; goto done; }
    bool ascii_header = true;
    for (size_t i = 0; i < header_size; ++i) if (header[i] >= 0x80) ascii_header = false;
    size_t probe_skip = 0;
    if (chunk->offset && !source_boundary) {
        while (probe_skip < length && probe_skip < 3 && ((uint8_t)chunk->text[probe_skip] & 0xC0U) == 0x80U) ++probe_skip;
    }
    if (!encoding_hint && !chunk->text_start_offset && ascii_header &&
        utf8_errors((const unsigned char *)chunk->text + probe_skip, length - probe_skip, false)) {
        chunk->encoding = APP_READER_GBK;
        error = read_converted(file, path, chunk->offset, limit, source_boundary, cancel, context, chunk); goto done;
    }
    const bool at_eof = chunk->offset + length == chunk->file_size;
    /* Slider seeks can land inside a UTF-8 codepoint. Skip only those leading
     * continuation bytes, reporting the adjustment in the original offset. */
    size_t skip = 0;
    if (chunk->offset) {
        while (skip < length && ((uint8_t)chunk->text[skip] & 0xC0U) == 0x80U) ++skip;
    }
    if (length - skip >= 3 && !memcmp(chunk->text + skip, "\xEF\xBB\xBF", 3)) skip += 3;
    if (skip) {
        memmove(chunk->text, chunk->text + skip, length - skip);
        length -= skip;
        chunk->offset += skip;
    }
    chunk->invalid_count = utf8_errors((const unsigned char *)chunk->text, length, at_eof);
    size_t clean = app_text_sanitize_utf8(chunk->text, length);
    if (at_eof && clean < length) {
        /* A damaged final sequence has no next chunk. Replace each original
         * byte so every displayed byte still has an unambiguous file offset. */
        memset(chunk->text + clean, '?', length - clean);
        clean = length;
        chunk->text[clean] = '\0';
    }
    chunk->next_offset = chunk->offset + clean;
done:
    fclose(file);
    if (error != ESP_OK) { free(chunk->text); chunk->text = NULL; free(chunk->source_offsets); chunk->source_offsets = NULL; }
    return error;
}
