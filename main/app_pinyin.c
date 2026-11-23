#include "app_pinyin.h"
#include "app_pinyin_extended.h"

#include <stdlib.h>
#include <string.h>

#define ARRAY_COUNT(array) (sizeof(array) / sizeof((array)[0]))

typedef struct {
    const char *spelling;
    const char *text;
} pinyin_entry_t;

static const pinyin_entry_t syllables[] = {
#include "assets/pinyin_unihan.inc"
};

static const pinyin_entry_t phrases[] = {
#include "assets/pinyin_phrases.inc"
#include "assets/pinyin_daily.inc"
};

#include "assets/pinyin_words_packed.inc"

typedef struct {
    app_pinyin_candidate_t *output;
    size_t capacity, skip, count, seen;
    bool has_more;
} candidate_page_t;

enum { SEGMENT_SYLLABLE, SEGMENT_PHRASE, SEGMENT_COMMON };
enum { STACK_SEGMENT_LIMIT = 16, SENTENCE_MIN_LETTERS = 23, PACKED_SPELLING_LIMIT = 31 };

/* One complete dictionary unit and what can follow it. */
typedef struct {
    const void *text;
    uint16_t cost;
    uint8_t length, covered, units, kind;
} segment_t;

size_t app_pinyin_syllable_count(void)
{
    return ARRAY_COUNT(syllables);
}

size_t app_pinyin_spelling_count(void)
{
    return ARRAY_COUNT(syllables) + ARRAY_COUNT(phrases);
}

const char *app_pinyin_spelling_at(size_t index)
{
    if (index < ARRAY_COUNT(syllables)) return syllables[index].spelling;
    index -= ARRAY_COUNT(syllables);
    return index < ARRAY_COUNT(phrases) ? phrases[index].spelling : NULL;
}

uint32_t app_pinyin_next_letters(const char *composition)
{
    if (!composition) return 0;
    size_t length = 0;
    while (length < APP_PINYIN_MAX_SPELLING &&
           composition[length] >= 'a' && composition[length] <= 'z') ++length;
    if (!length || length == APP_PINYIN_MAX_SPELLING || composition[length]) return 0;

    /* A partial suffix contributes only if every preceding syllable is valid.
     * One complete segmentation is enough to keep the next key unconstrained. */
    bool boundary[APP_PINYIN_MAX_SPELLING + 1] = {true};
    uint32_t letters = 0;
    for (size_t start = 0; start < length; ++start) {
        if (!boundary[start]) continue;
        const char *tail = composition + start;
        for (size_t i = 0; i < ARRAY_COUNT(syllables); ++i) {
            const char *syllable = syllables[i].spelling;
            if (*syllable != *tail) continue;
            size_t matched = 1;
            while (syllable[matched] && syllable[matched] == tail[matched]) ++matched;
            if (!syllable[matched]) {
                if (start + matched == length) return 0;
                boundary[start + matched] = true;
            } else if (!tail[matched]) {
                letters |= UINT32_C(1) << (syllable[matched] - 'a');
            }
        }
    }
    return letters;
}

size_t app_pinyin_common_phrase_count(void)
{
    return COMMON_PHRASE_COUNT;
}

const char *app_pinyin_common_phrase_spelling_at(size_t index)
{
    if (index >= COMMON_PHRASE_COUNT) return NULL;
    const size_t offset = common_spelling_bases[index / COMMON_BLOCK_SIZE] + common_offsets[index].spelling;
    return common_spellings + offset;
}

static const uint8_t *common_word_at(size_t index)
{
    const size_t offset = common_word_bases[index / COMMON_BLOCK_SIZE] + common_offsets[index].words;
    return common_word_data + offset;
}

uint8_t app_pinyin_common_phrase_priority(size_t index)
{
    return index < COMMON_PHRASE_COUNT ? (uint8_t)((*common_word_at(index) & 63U) / 4U) : 0;
}

static const pinyin_entry_t *find_exact(const pinyin_entry_t *entries, size_t count,
                                        const char *spelling)
{
    for (size_t i = 0; i < count; ++i)
        if (strcmp(entries[i].spelling, spelling) == 0) return &entries[i];
    return NULL;
}

static size_t common_lower_bound(const char *spelling, size_t length, size_t low)
{
    size_t high = COMMON_PHRASE_COUNT;
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        const char *key = app_pinyin_common_phrase_spelling_at(middle);
        if (strncmp(key, spelling, length) < 0) low = middle + 1;
        else high = middle;
    }
    return low;
}

static size_t find_common_phrase(const char *spelling, size_t length)
{
    const size_t index = common_lower_bound(spelling, length, 0);
    const char *key = app_pinyin_common_phrase_spelling_at(index);
    return key && !strncmp(key, spelling, length) && !key[length] ? index : COMMON_PHRASE_COUNT;
}

static bool add_candidate(candidate_page_t *page, const char *text, size_t bytes, size_t consumed)
{
    if (!bytes || bytes >= APP_PINYIN_CANDIDATE_BYTES) return true;
    ++page->seen;
    if (page->skip) {
        --page->skip;
    } else if (page->count < page->capacity) {
        app_pinyin_candidate_t *candidate = &page->output[page->count++];
        memcpy(candidate->text, text, bytes);
        candidate->text[bytes] = '\0';
        candidate->consumed = (uint8_t)consumed;
    } else {
        page->has_more = true;
        return false;
    }
    return true;
}

static size_t character_bytes(const char *text)
{
    const unsigned lead = (unsigned char)text[0];
    if (lead >= 0xf0) return 4;
    if (lead >= 0xe0) return 3;
    if (lead >= 0xc0) return 2;
    return lead != 0;
}

static void add_syllable(candidate_page_t *page, const char *text, size_t consumed,
                         const char *skip_first)
{
    for (; *text; text += character_bytes(text)) {
        const size_t bytes = character_bytes(text);
        if (skip_first && strlen(skip_first) == bytes && !memcmp(text, skip_first, bytes)) continue;
        if (!add_candidate(page, text, bytes, consumed)) return;
    }
}

static size_t primary_phrase_bytes(const char *text)
{
    const char *end = strchr(text, '|');
    return end ? (size_t)(end - text) : strlen(text);
}

static void add_phrases(candidate_page_t *page, const char *text, size_t consumed)
{
    for (;;) {
        const size_t bytes = primary_phrase_bytes(text);
        if (!add_candidate(page, text, bytes, consumed) || !text[bytes]) return;
        text += bytes + 1;
    }
}

static size_t common_word_length(const uint8_t *word)
{
    return (*word >> 6) + 2U;
}

/* The dictionary generator admits only BMP CJK glyphs shared by both fonts. */
static size_t unpack_common_word(const uint8_t *word, char *utf8)
{
    const size_t length = common_word_length(word);
    for (size_t i = 0; i < length; ++i) {
        const uint16_t cp = word[1 + 2 * i] | ((uint16_t)word[2 + 2 * i] << 8);
        utf8[3 * i] = (char)(0xe0U | (cp >> 12));
        utf8[3 * i + 1] = (char)(0x80U | ((cp >> 6) & 63U));
        utf8[3 * i + 2] = (char)(0x80U | (cp & 63U));
    }
    return 3 * length;
}

static bool known_phrase_word(const char *phrases_text, const char *word, size_t bytes)
{
    while (phrases_text) {
        const size_t length = primary_phrase_bytes(phrases_text);
        if (length == bytes && !memcmp(phrases_text, word, bytes)) return true;
        if (!phrases_text[length]) break;
        phrases_text += length + 1;
    }
    return false;
}

static void add_packed_words(candidate_page_t *page, const uint8_t *first, const uint8_t *end,
                             size_t consumed, const char *phrases_text)
{
    for (const uint8_t *word = first; word < end; word += 1 + 2 * common_word_length(word)) {
        char text[5 * 3];
        const size_t bytes = unpack_common_word(word, text);
        if (!known_phrase_word(phrases_text, text, bytes) && !add_candidate(page, text, bytes, consumed)) return;
    }
}

static bool better_segment(const segment_t *best, size_t length, segment_t rest, uint16_t cost,
                           bool sentence)
{
    const unsigned units = rest.units + 1U;
    if (rest.covered != best->covered) return rest.covered > best->covered;
    if (sentence && cost != best->cost) return cost < best->cost;
    if (units != best->units) return units < best->units;
    if (cost != best->cost) return cost < best->cost;
    return length > best->length;
}

static unsigned syllable_sentence_cost(const char *primary)
{
    /* Common standalone function words are valid boundaries in a sentence.
     * Make them less costly than fragments such as a rare bare syllable. */
    static const char common[] = "的是在也我你他她了到从把和不很就又可去有要能会给请吗呢再都只才";
    for (const char *word = common; *word; word += 3)
        if (!strncmp(word, primary, 3)) return 16;
    return 48;
}

static void try_segments(const pinyin_entry_t *entries, size_t count, bool phrase,
                         const char *spelling, size_t position, size_t end, segment_t *segments,
                         bool sentence)
{
    for (size_t i = 0; i < count; ++i) {
        const pinyin_entry_t *entry = &entries[i];
        if (entry->spelling[0] != spelling[position]) continue;
        const size_t length = strlen(entry->spelling);
        if (length > end - position || memcmp(entry->spelling, spelling + position, length)) continue;
        const segment_t rest = segments[position + length];
        const unsigned cost_here = sentence ? (phrase ? 10 : syllable_sentence_cost(entry->text)) :
            (phrase ? 0 : 40);
        const uint16_t cost = rest.cost + cost_here;
        if (better_segment(&segments[position], length, rest, cost, sentence))
            segments[position] = (segment_t){
                entry->text, cost, (uint8_t)length, rest.covered, (uint8_t)(rest.units + 1),
                phrase ? SEGMENT_PHRASE : SEGMENT_SYLLABLE,
            };
    }
}

static void try_packed_segments(const char *spelling, size_t position, size_t end,
                                segment_t *segments, bool sentence, bool extended)
{
    size_t index = 0;
    for (size_t length = 2; length <= end - position && length <= PACKED_SPELLING_LIMIT; ++length) {
        index = extended ? app_pinyin_extended_lower_bound(spelling + position, length, index) :
            common_lower_bound(spelling + position, length, index);
        const char *key = extended ? app_pinyin_extended_key(index) : app_pinyin_common_phrase_spelling_at(index);
        if (!key || strncmp(key, spelling + position, length)) break;
        if (key[length]) continue;
        const segment_t rest = segments[position + length];
        const uint8_t *word = extended ? app_pinyin_extended_words(index, NULL) : common_word_at(index);
        const unsigned base_cost = (sentence ? 28 : 16) + (extended ? 12 : 0);
        const unsigned priority = (*word & 63U) / 4U;
        const uint16_t cost = rest.cost + base_cost - priority;
        if (better_segment(&segments[position], length, rest, cost, sentence))
            segments[position] = (segment_t){word, cost, (uint8_t)length, rest.covered,
                                             (uint8_t)(rest.units + 1), SEGMENT_COMMON};
    }
}

static const pinyin_entry_t *first_syllable(const char *spelling, size_t length,
                                             const segment_t *segments)
{
    const pinyin_entry_t *best = NULL;
    size_t best_length = 0;
    for (size_t i = 0; i < ARRAY_COUNT(syllables); ++i) {
        const pinyin_entry_t *entry = &syllables[i];
        if (entry->spelling[0] != spelling[0]) continue;
        const size_t bytes = strlen(entry->spelling);
        if (bytes >= length || memcmp(spelling, entry->spelling, bytes)) continue;
        if (!best || segments[bytes].covered > segments[best_length].covered ||
            (segments[bytes].covered == segments[best_length].covered && bytes > best_length)) {
            best = entry;
            best_length = bytes;
        }
    }
    return best;
}

static size_t copy_segment(const segment_t *segment, char *out, size_t capacity)
{
    const size_t bytes = segment->kind == SEGMENT_COMMON ? 3 * common_word_length(segment->text) :
        segment->kind == SEGMENT_PHRASE ? primary_phrase_bytes(segment->text) : character_bytes(segment->text);
    if (bytes >= capacity) return 0;
    if (segment->kind == SEGMENT_COMMON) unpack_common_word(segment->text, out);
    else memcpy(out, segment->text, bytes);
    return bytes;
}

static void fill_segments(const char *spelling, size_t length, segment_t *segments, bool sentence)
{
    /* Work backwards so a known word is compared with the best continuation. */
    const bool have_extension = app_pinyin_extended_count() != 0;
    segments[length].covered = (uint8_t)length;
    for (size_t position = length; position-- > 0;) {
        segments[position].covered = (uint8_t)position;
        try_segments(phrases, ARRAY_COUNT(phrases), true, spelling, position, length, segments, sentence);
        try_packed_segments(spelling, position, length, segments, sentence, false);
        if (have_extension) try_packed_segments(spelling, position, length, segments, sentence, true);
        try_segments(syllables, ARRAY_COUNT(syllables), false, spelling, position, length, segments, sentence);
    }
}

static void add_completions(candidate_page_t *page, const pinyin_entry_t *entries, size_t count,
                            const char *spelling, size_t length, bool phrase);

static void add_segments(candidate_page_t *page, const char *spelling, size_t length,
                         bool complete_phrase, const segment_t *segments, bool sentence)
{
    char composed[APP_PINYIN_CANDIDATE_BYTES] = {0};
    size_t consumed = 0;
    if (!complete_phrase) {
        size_t bytes_used = 0;
        while (segments[consumed].length) {
            const segment_t *head = &segments[consumed];
            const size_t take = copy_segment(head, composed + bytes_used, sizeof(composed) - bytes_used);
            if (!take) break;
            bytes_used += take;
            consumed += head->length;
        }
        if (!add_candidate(page, composed, bytes_used, consumed)) return;
    }
    const segment_t *head = &segments[0];
    if (sentence && head->length && head->length < consumed && head->kind != SEGMENT_SYLLABLE) {
        char prefix[APP_PINYIN_CANDIDATE_BYTES];
        const size_t bytes = copy_segment(head, prefix, sizeof(prefix));
        if (!add_candidate(page, prefix, bytes, head->length)) return;
    }
    const pinyin_entry_t *initial = first_syllable(spelling, length, segments);
    const size_t first = initial ? strlen(initial->spelling) : 0;
    const char *remaining = initial ? initial->text : NULL;
    if (complete_phrase && length >= 3) {
        if (remaining) {
            const size_t bytes = character_bytes(remaining);
            if (!add_candidate(page, remaining, bytes, first)) return;
            remaining += bytes;
        }
        add_completions(page, phrases, ARRAY_COUNT(phrases), spelling, length, true);
    }
    if (remaining && !page->has_more)
        add_syllable(page, remaining, first, consumed == first ? composed : NULL);
}

static void add_segmented(candidate_page_t *page, const char *spelling, size_t length,
                          bool complete_phrase)
{
    segment_t local[STACK_SEGMENT_LIMIT + 1] = {0};
    segment_t *segments = length <= STACK_SEGMENT_LIMIT ? local : calloc(length + 1, sizeof(*segments));
    if (!segments) return;
    const bool sentence = length >= SENTENCE_MIN_LETTERS;
    fill_segments(spelling, length, segments, sentence);
    add_segments(page, spelling, length, complete_phrase, segments, sentence);
    if (segments != local) free(segments);
}

static void add_completions(candidate_page_t *page, const pinyin_entry_t *entries, size_t count,
                            const char *spelling, size_t length, bool phrase)
{
    unsigned offers = 0;
    for (size_t i = 0; i < count && offers < (phrase ? 3U : 16U); ++i) {
        const pinyin_entry_t *entry = &entries[i];
        if (strncmp(entry->spelling, spelling, length) || !entry->spelling[length]) continue;
        const size_t bytes = phrase ? primary_phrase_bytes(entry->text) : character_bytes(entry->text);
        if (!add_candidate(page, entry->text, bytes, length)) return;
        ++offers;
    }
}

size_t app_pinyin_lookup(const char *spelling, size_t offset, app_pinyin_candidate_t *out,
                        size_t capacity, bool *has_more)
{
    if (has_more) *has_more = false;
    if (!spelling || (!out && capacity)) return 0;
    size_t length = 0;
    while (length < APP_PINYIN_MAX_SPELLING && spelling[length] >= 'a' && spelling[length] <= 'z')
        ++length;
    if (!length || spelling[length]) return 0;

    candidate_page_t page = {.output = out, .capacity = capacity, .skip = offset};
    const pinyin_entry_t *single = find_exact(syllables, ARRAY_COUNT(syllables), spelling);
    const pinyin_entry_t *phrase = find_exact(phrases, ARRAY_COUNT(phrases), spelling);
    const size_t common_index = find_common_phrase(spelling, length);
    const bool common = common_index != COMMON_PHRASE_COUNT;
    const size_t ext_index = app_pinyin_extended_lower_bound(spelling, length, 0);
    const char *ext_key = app_pinyin_extended_key(ext_index);
    const bool extended = ext_key && !strcmp(ext_key, spelling);
    const char *single_tail = single ? single->text : NULL;
    if (single_tail && (phrase || common || extended || length >= 3)) {
        /* A trace such as xian can mean either 先 or 西安. Keep the frequent
         * single characters first without burying the word in later pages. */
        for (unsigned i = 0; i < 3 && *single_tail; ++i) {
            const size_t bytes = character_bytes(single_tail);
            if (!add_candidate(&page, single_tail, bytes, length)) break;
            single_tail += bytes;
        }
    }
    if (phrase && !page.has_more) add_phrases(&page, phrase->text, length);
    if (common && !page.has_more)
        add_packed_words(&page, common_word_at(common_index), common_word_at(common_index + 1), length,
                         phrase ? phrase->text : NULL);
    if (extended && !page.has_more) {
        const uint8_t *end;
        const uint8_t *first = app_pinyin_extended_words(ext_index, &end);
        if (first) add_packed_words(&page, first, end, length, phrase ? phrase->text : NULL);
    }
    if (!page.has_more) {
        if (single_tail) {
            if (length >= 3) add_completions(&page, phrases, ARRAY_COUNT(phrases), spelling, length, true);
            if (!page.has_more) add_syllable(&page, single_tail, length, NULL);
        } else {
            if (!phrase && !common && !extended && length >= 3)
                add_completions(&page, phrases, ARRAY_COUNT(phrases), spelling, length, true);
            if (!page.has_more) add_segmented(&page, spelling, length, phrase || common || extended);
            if (!page.seen)
                add_completions(&page, syllables, ARRAY_COUNT(syllables), spelling, length, false);
        }
    }
    if (has_more) *has_more = page.has_more;
    return page.count;
}
