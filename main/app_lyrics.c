#include "app_lyrics.h"

#include <stdlib.h>
#include <string.h>
#include <strings.h>

enum { LYRIC_CUE_LIMIT = 384 };

typedef struct {
    uint32_t ms;
    uint16_t offset, order;
} lyric_cue_t;

struct app_lyrics {
    size_t count;
    lyric_cue_t cues[LYRIC_CUE_LIMIT];
    char text[];
};

static bool horizontal_space(char ch)
{
    return ch == ' ' || ch == '\t';
}

static bool digits(const char **text, const char *end, unsigned max_digits,
                   unsigned *value, unsigned *count)
{
    const char *cursor = *text;
    *count = *value = 0;
    while (cursor < end && *cursor >= '0' && *cursor <= '9') {
        if (*count == max_digits) return false;
        *value = *value * 10U + (unsigned)(*cursor++ - '0');
        ++*count;
    }
    *text = cursor;
    return *count != 0;
}

static bool timestamp(const char *tag, const char *end, uint32_t *ms)
{
    unsigned minutes, seconds, count, fraction = 0;
    if (!digits(&tag, end, 4, &minutes, &count) || tag == end || *tag++ != ':' ||
        !digits(&tag, end, 2, &seconds, &count) || seconds >= 60) return false;
    if (tag != end) {
        if (*tag != '.' && *tag != ':') return false;
        ++tag;
        if (!digits(&tag, end, 3, &fraction, &count)) return false;
        for (; count < 3; ++count) fraction *= 10U;
    }
    if (tag != end) return false;
    *ms = (minutes * 60U + seconds) * 1000U + fraction;
    return true;
}

static void read_offset(const char *tag, const char *end, int32_t *out)
{
    static const char key[] = "offset:";
    if ((size_t)(end - tag) <= sizeof(key) - 1 || strncasecmp(tag, key, sizeof(key) - 1)) return;
    tag += sizeof(key) - 1;
    while (tag < end && horizontal_space(*tag)) ++tag;
    const bool negative = tag < end && *tag == '-';
    if (tag < end && (*tag == '+' || *tag == '-')) ++tag;
    unsigned value, count;
    if (!digits(&tag, end, 7, &value, &count)) return;
    while (tag < end && horizontal_space(*tag)) ++tag;
    if (tag == end) *out = negative ? -(int32_t)value : (int32_t)value;
}

static void parse_line(app_lyrics_t *lyrics, char *line, char *end, int32_t *offset)
{
    size_t start = lyrics->count;
    while (line < end && horizontal_space(*line)) ++line;
    while (line < end && *line == '[') {
        const char *tag_end = memchr(line + 1, ']', (size_t)(end - line - 1));
        if (!tag_end) break;
        uint32_t ms;
        if (timestamp(line + 1, tag_end, &ms)) {
            if (lyrics->count < LYRIC_CUE_LIMIT) {
                const size_t index = lyrics->count++;
                lyrics->cues[index] = (lyric_cue_t){.ms = ms, .order = (uint16_t)index};
            }
        } else if (start == lyrics->count) {
            read_offset(line + 1, tag_end, offset);
        } else break;
        line = (char *)tag_end + 1;
    }
    while (line < end && horizontal_space(*line)) ++line;
    while (end > line && horizontal_space(end[-1])) --end;
    *end = '\0';
    for (; start < lyrics->count; ++start)
        lyrics->cues[start].offset = (uint16_t)(line - lyrics->text);
}

static int compare_cues(const void *a, const void *b)
{
    const lyric_cue_t *left = a, *right = b;
    if (left->ms < right->ms) return -1;
    if (left->ms > right->ms) return 1;
    return (int)left->order - (int)right->order;
}

app_lyrics_t *app_lyrics_parse(const char *text, bool complete)
{
    if (!text) return NULL;
    const size_t length = strlen(text);
    if (!length || length > APP_LYRICS_READ_LIMIT) return NULL;
    app_lyrics_t *lyrics = malloc(sizeof(*lyrics) + length + 1);
    if (!lyrics) return NULL;
    lyrics->count = 0;
    memcpy(lyrics->text, text, length + 1);
    int32_t offset = 0;
    char *const limit = lyrics->text + length;
    char *line = lyrics->text;
    if (length >= 3 && !memcmp(line, "\xef\xbb\xbf", 3)) line += 3;
    while (line < limit) {
        char *end = strpbrk(line, "\r\n");
        if (!end && !complete) break;
        char *next = end ? end + 1 : limit;
        if (end && *end == '\r' && next < limit && *next == '\n') ++next;
        parse_line(lyrics, line, end ? end : limit, &offset);
        line = next;
    }
    if (!lyrics->count) { free(lyrics); return NULL; }
    for (size_t i = 0; i < lyrics->count; ++i) {
        /* In LRC a positive [offset:] advances the displayed lyric. */
        const int64_t adjusted = (int64_t)lyrics->cues[i].ms - offset;
        lyrics->cues[i].ms = adjusted > 0 ? (uint32_t)adjusted : 0;
    }
    qsort(lyrics->cues, lyrics->count, sizeof(*lyrics->cues), compare_cues);
    /* Keep the earliest text at each instant. A blank cue at its own
     * timestamp remains useful to clear an instrumental interval. */
    size_t unique = 0;
    for (size_t i = 0; i < lyrics->count; ++i) {
        const lyric_cue_t cue = lyrics->cues[i];
        if (!unique || lyrics->cues[unique - 1].ms != cue.ms) lyrics->cues[unique++] = cue;
        else if (!lyrics->text[lyrics->cues[unique - 1].offset]) lyrics->cues[unique - 1] = cue;
    }
    lyrics->count = unique;
    return lyrics;
}

size_t app_lyrics_count(const app_lyrics_t *lyrics)
{
    return lyrics ? lyrics->count : 0;
}

size_t app_lyrics_at(const app_lyrics_t *lyrics, uint32_t position_ms)
{
    size_t lower = 0, upper = app_lyrics_count(lyrics);
    while (lower < upper) {
        const size_t mid = lower + (upper - lower) / 2;
        if (lyrics->cues[mid].ms <= position_ms) lower = mid + 1;
        else upper = mid;
    }
    return lower ? lower - 1 : SIZE_MAX;
}

const char *app_lyrics_line(const app_lyrics_t *lyrics, size_t index)
{
    return index < app_lyrics_count(lyrics) ? lyrics->text + lyrics->cues[index].offset : "";
}
