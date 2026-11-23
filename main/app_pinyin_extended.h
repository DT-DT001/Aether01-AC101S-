#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(ESP_PLATFORM) || defined(APP_PINYIN_EXTENDED_TEST)
/* Initialize once at boot. With absent or incompatible data the built-in
 * dictionary remains available. An opened mapping lives until reboot. */
bool app_pinyin_extended_init(void);
size_t app_pinyin_extended_count(void);
const char *app_pinyin_extended_key(size_t index);
const uint8_t *app_pinyin_extended_words(size_t index, const uint8_t **end);
#else
static inline size_t app_pinyin_extended_count(void) { return 0; }
static inline const char *app_pinyin_extended_key(size_t index) { (void)index; return NULL; }
static inline const uint8_t *app_pinyin_extended_words(size_t index, const uint8_t **end)
{
    (void)index;
    if (end) *end = NULL;
    return NULL;
}
#endif

static inline size_t app_pinyin_extended_lower_bound(const char *key, size_t length, size_t low)
{
    size_t high = app_pinyin_extended_count();
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const char *candidate = app_pinyin_extended_key(middle);
        size_t i = 0;
        while (i < length && candidate[i] && candidate[i] == key[i]) ++i;
        if (i < length && candidate[i] < key[i]) low = middle + 1;
        else high = middle;
    }
    return low < high ? low : high;
}

static inline uint8_t app_pinyin_extended_priority(size_t index)
{
    const uint8_t *record = app_pinyin_extended_words(index, NULL);
    return record ? (uint8_t)((record[0] & 63U) / 4U) : 0;
}

#ifdef APP_PINYIN_EXTENDED_TEST
bool app_pinyin_extended_test_load(const uint8_t *data, size_t size);
#endif
