#pragma once

#include <stddef.h>
#include <stdint.h>

enum {
    APP_PINYIN_SWIPE_MAX_POINTS = 160,
    APP_PINYIN_SWIPE_MAX_SPELLING = 72,
    APP_PINYIN_SWIPE_MAX_MATCHES = 8,
};

typedef struct { int16_t x, y; } app_pinyin_swipe_point_t;
typedef const char *(*app_pinyin_swipe_word_at_t)(size_t index, void *context);

/* Shorten a full trace in place while retaining its endpoints and turns. */
size_t app_pinyin_swipe_compact(app_pinyin_swipe_point_t *points, size_t count);

/* Compare one contact trace to known lowercase spellings. The caller supplies
 * centers a..z in the same coordinates as the points. Returned spellings are
 * borrowed from the dictionary, ordered by fit, and may be empty on rejection. */
size_t app_pinyin_swipe_match(const app_pinyin_swipe_point_t *points, size_t point_count,
                             const app_pinyin_swipe_point_t centers[26],
                             size_t word_count, app_pinyin_swipe_word_at_t word_at,
                             void *context, const char **matches, size_t match_capacity);

/* Recognize known spellings and plausible combinations of valid syllables.
 * The results are copied to caller-owned, NUL-terminated buffers. */
size_t app_pinyin_swipe_decode(const app_pinyin_swipe_point_t *points, size_t point_count,
                              const app_pinyin_swipe_point_t centers[26],
                              char matches[][APP_PINYIN_SWIPE_MAX_SPELLING + 1], size_t match_capacity);

/* Use the next-letter mask only to break geometric near-ties at touch-down. */
size_t app_pinyin_swipe_decode_context(const app_pinyin_swipe_point_t *points, size_t point_count,
                                      const app_pinyin_swipe_point_t centers[26], uint32_t next_letters,
                                      char matches[][APP_PINYIN_SWIPE_MAX_SPELLING + 1], size_t match_capacity);
