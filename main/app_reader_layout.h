#pragma once

#include <stddef.h>
#include <stdint.h>

typedef uint16_t (*app_reader_glyph_width_t)(uint32_t codepoint,
                                             uint32_t next_codepoint,
                                             void *context);
uint32_t app_reader_codepoint(const char *source, size_t remaining, size_t *bytes);

typedef struct {
    size_t consumed; /**< Bytes from the original UTF-8 source, including CRLF. */
    size_t written;  /**< Output bytes, excluding the terminating NUL. */
    unsigned lines;
} app_reader_page_t;

/** Build one page with explicit line breaks using actual glyph advances.
 * The caller retains the source and supplies output_capacity writable bytes.
 * Neither an automatic wrap nor an output boundary splits a UTF-8 codepoint.
 */
app_reader_page_t app_reader_layout_page(const char *source, size_t length,
                                         char *output, size_t output_capacity,
                                         unsigned width, unsigned height,
                                         unsigned line_height, unsigned line_space,
                                         app_reader_glyph_width_t glyph_width,
                                         void *context);
