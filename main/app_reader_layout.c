#include "app_reader_layout.h"

#include <stdbool.h>
#include <limits.h>
#include <string.h>

uint32_t app_reader_codepoint(const char *source, size_t remaining, size_t *bytes)
{
    if (!remaining) { *bytes = 0; return 0; }
    const unsigned char *p = (const unsigned char *)source;
    unsigned count = p[0] < 0x80 ? 1 : p[0] >= 0xC2 && p[0] <= 0xDF ? 2 :
                     p[0] >= 0xE0 && p[0] <= 0xEF ? 3 :
                     p[0] >= 0xF0 && p[0] <= 0xF4 ? 4 : 0;
    if (!count || count > remaining) { *bytes = 1; return '?'; }
    uint32_t value = p[0] & (count == 1 ? 0x7FU : (1U << (7U - count)) - 1U);
    for (unsigned i = 1; i < count; ++i) {
        if ((p[i] & 0xC0U) != 0x80U) { *bytes = 1; return '?'; }
        value = (value << 6) | (p[i] & 0x3FU);
    }
    if ((count == 2 && value < 0x80U) || (count == 3 && value < 0x800U) ||
        (count == 4 && value < 0x10000U) || value > 0x10FFFFU ||
        (value >= 0xD800U && value <= 0xDFFFU)) {
        *bytes = 1; return '?';
    }
    *bytes = count;
    return value;
}

static bool cannot_start_line(uint32_t codepoint)
{
    switch (codepoint) {
    case ',': case '.': case '!': case '?': case ':': case ';':
    case ')': case ']': case '}':
    case 0x3001: case 0x3002: case 0xFF0C: case 0xFF0E:
    case 0xFF01: case 0xFF1F: case 0xFF1A: case 0xFF1B:
    case 0x2019: case 0x201D: case 0xFF09: case 0x3009:
    case 0x300B: case 0x300D: case 0x300F: case 0x3011:
    case 0x3015: case 0x3017: case 0x3019: case 0x301B:
        return true;
    default:
        return false;
    }
}

app_reader_page_t app_reader_layout_page(const char *source, size_t length,
                                         char *output, size_t output_capacity,
                                         unsigned width, unsigned height,
                                         unsigned line_height, unsigned line_space,
                                         app_reader_glyph_width_t glyph_width,
                                         void *context)
{
    app_reader_page_t page = {0};
    if (output && output_capacity) output[0] = '\0';
    if (!source || !output || output_capacity < 2 || !glyph_width ||
        !width || !line_height || line_space > UINT_MAX - line_height ||
        height < line_height) return page;
    const unsigned max_lines = 1U + (height - line_height) / (line_height + line_space);
    unsigned x = 0;
    page.lines = 1;
    while (page.consumed < length && source[page.consumed]) {
        size_t bytes;
        uint32_t codepoint = app_reader_codepoint(source + page.consumed, length - page.consumed, &bytes);
        if (codepoint == 0xFEFFU) { page.consumed += bytes; continue; }
        if (codepoint == '\r' || codepoint == '\n') {
            if (codepoint == '\r' && page.consumed + bytes < length &&
                source[page.consumed + bytes] == '\n') ++bytes;
            if (page.lines >= max_lines) {
                page.consumed += bytes;
                break;
            }
            if (page.written + 1U >= output_capacity) break;
            output[page.written++] = '\n';
            ++page.lines;
            x = 0;
            page.consumed += bytes;
            continue;
        }
        size_t next_bytes;
        uint32_t next = app_reader_codepoint(source + page.consumed + bytes,
                               length - page.consumed - bytes, &next_bytes);
        (void)next_bytes;
        const bool tab = codepoint == '\t';
        const unsigned advance = tab ? (unsigned)glyph_width(' ', ' ', context) * 4U :
                                       glyph_width(codepoint, next, context);
        const unsigned keep_with_next = cannot_start_line(next) ?
                                          (unsigned)glyph_width(next, 0, context) : 0;
        const size_t output_bytes = tab ? 4U : codepoint == '?' && bytes == 1 &&
                                    (unsigned char)source[page.consumed] >= 0x80U ? 1U : bytes;
        if (x && (advance > width - x || keep_with_next > width - x - advance)) {
            if (page.lines >= max_lines || page.written + 1U >= output_capacity) break;
            output[page.written++] = '\n';
            ++page.lines;
            x = 0;
        }
        if (page.written + output_bytes >= output_capacity) break;
        if (tab) memset(output + page.written, ' ', output_bytes);
        else if (output_bytes == 1 && codepoint == '?') output[page.written] = '?';
        else memcpy(output + page.written, source + page.consumed, output_bytes);
        page.written += output_bytes;
        page.consumed += bytes;
        x = advance > width - x ? width : x + advance;
    }
    output[page.written] = '\0';
    return page;
}
