#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { APP_PINYIN_MAX_SPELLING = 72, APP_PINYIN_CANDIDATE_BYTES = 72 };

typedef struct {
    char text[APP_PINYIN_CANDIDATE_BYTES];
    uint8_t consumed; /* Bytes of the ASCII composition replaced by this candidate. */
} app_pinyin_candidate_t;

/* Lookup is offline. Offset counts candidates, not bytes. Spelling is lower-case ASCII. */
size_t app_pinyin_lookup(const char *spelling, size_t offset, app_pinyin_candidate_t *out,
                        size_t capacity, bool *has_more);

/* Complete spellings only, for gesture recognition. Out-of-range returns NULL. */
size_t app_pinyin_syllable_count(void);
size_t app_pinyin_spelling_count(void);
const char *app_pinyin_spelling_at(size_t index);

/* Larger frequency-ranked vocabulary for complete gestures and text lookup. */
size_t app_pinyin_common_phrase_count(void);
const char *app_pinyin_common_phrase_spelling_at(size_t index);
uint8_t app_pinyin_common_phrase_priority(size_t index); /* 0..15, higher means more frequent. */
