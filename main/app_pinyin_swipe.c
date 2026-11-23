#include "app_pinyin_swipe.h"
#include "app_pinyin.h"
#include "app_pinyin_extended.h"

#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

enum {
    TRACE_SAMPLES = 16,
    ALIGNMENT_BAND = 3,
    MAX_WORDS = 1024,
    MAX_COORDINATE = 4095,
    WORD_MAX_SPELLING = 31,
    CHAIN_UNITS = 5,
    CHAIN_FIT_LIMIT = 2800,
    SENTENCE_CHAIN_UNITS = 18,
    SENTENCE_FIT_LIMIT = 17000,
    SENTENCE_ALTERNATIVES = 4,
    SENTENCE_ARC_KEY_WIDTHS = 64,
    SENTENCE_FALLBACK_KEY_WIDTHS = 36,
};

typedef app_pinyin_swipe_point_t point_t;

static uint32_t squared_distance(point_t a, point_t b)
{
    const int32_t dx = (int32_t)a.x - b.x;
    const int32_t dy = (int32_t)a.y - b.y;
    return (uint32_t)(dx * dx + dy * dy);
}

static uint32_t segment_length_eighths(point_t a, point_t b)
{
    const unsigned dx = abs((int)a.x - b.x);
    const unsigned dy = abs((int)a.y - b.y);
    const unsigned longer = dx > dy ? dx : dy;
    const unsigned shorter = dx > dy ? dy : dx;
    return 8U * longer + 3U * shorter;
}

static uint32_t segment_length(point_t a, point_t b)
{
    /* Integer approximation of Euclidean arc length. */
    return segment_length_eighths(a, b) / 8U;
}

static uint32_t removal_cost(point_t a, point_t b, point_t c)
{
    const uint32_t chord = segment_length_eighths(a, c);
    const uint32_t traveled = segment_length_eighths(a, b) + segment_length_eighths(b, c);
    const int32_t ab_x = (int32_t)b.x - a.x, ab_y = (int32_t)b.y - a.y;
    const int32_t ac_x = (int32_t)c.x - a.x, ac_y = (int32_t)c.y - a.y;
    const int64_t area = (int64_t)ab_x * ac_y - (int64_t)ab_y * ac_x;
    const uint32_t deviation = chord ? (uint32_t)llabs(area) / ((chord + 7U) / 8U) : 0;
    /* The integer arc estimate alone is flat for bends within one octant.
     * Charge for the loss of the turn and for the wider sampling gap. */
    return traveled - chord + deviation * 24U + chord / 4U;
}

size_t app_pinyin_swipe_compact(point_t *points, size_t count)
{
    if (count < 4) return count;
    const size_t target = count - (count - 1) / 3;
    while (count > target) {
        size_t lightest = 1;
        uint32_t best = UINT32_MAX;
        for (size_t i = 1; i + 1 < count; ++i) {
            const uint32_t cost = removal_cost(points[i - 1], points[i], points[i + 1]);
            if (cost < best) {
                best = cost;
                lightest = i;
            }
        }
        /* Recheck neighbors after removing a point. This preserves bends
         * anywhere in the trace even as older points are compacted again. */
        --count;
        memmove(points + lightest, points + lightest + 1, (count - lightest) * sizeof(*points));
    }
    return count;
}

static uint32_t path_length(const point_t *path, size_t count)
{
    uint32_t length = 0;
    for (size_t i = 1; i < count; ++i) length += segment_length(path[i - 1], path[i]);
    return length;
}

static uint32_t resample(const point_t *path, size_t count, point_t *points, size_t samples)
{
    const uint32_t length = path_length(path, count);
    if (!length) return 0;

    points[0] = path[0];
    points[samples - 1] = path[count - 1];
    uint32_t walked = 0;
    uint32_t segment = segment_length(path[0], path[1]);
    size_t end = 1;
    for (size_t i = 1; i + 1 < samples; ++i) {
        const uint32_t target = length * i / (samples - 1);
        while (!segment || walked + segment < target) {
            walked += segment;
            ++end;
            segment = segment_length(path[end - 1], path[end]);
        }
        const point_t a = path[end - 1], b = path[end];
        const int32_t offset = (int32_t)(target - walked);
        points[i] = (point_t){
            .x = a.x + ((int32_t)b.x - a.x) * offset / (int32_t)segment,
            .y = a.y + ((int32_t)b.y - a.y) * offset / (int32_t)segment,
        };
    }
    return length;
}

static uint32_t alignment_cost(const point_t actual[TRACE_SAMPLES], const point_t ideal[TRACE_SAMPLES])
{
    const uint32_t infinity = UINT32_MAX / 4U;
    uint32_t rows[2][TRACE_SAMPLES + 1];
    rows[0][0] = 0;
    for (unsigned j = 1; j <= TRACE_SAMPLES; ++j) rows[0][j] = infinity;
    for (unsigned i = 1; i <= TRACE_SAMPLES; ++i) {
        uint32_t *current = rows[i & 1U];
        const uint32_t *previous = rows[(i - 1U) & 1U];
        const unsigned first = i > ALIGNMENT_BAND ? i - ALIGNMENT_BAND : 1;
        const unsigned last = i + ALIGNMENT_BAND < TRACE_SAMPLES ? i + ALIGNMENT_BAND : TRACE_SAMPLES;
        current[first - 1U] = infinity;
        if (last < TRACE_SAMPLES) current[last + 1U] = infinity;
        for (unsigned j = first; j <= last; ++j) {
            uint32_t cost = previous[j - 1U];
            if (previous[j] < cost) cost = previous[j];
            if (current[j - 1U] < cost) cost = current[j - 1U];
            current[j] = cost + squared_distance(actual[i - 1U], ideal[j - 1U]);
        }
    }
    return rows[TRACE_SAMPLES & 1U][TRACE_SAMPLES];
}

static bool spans_keys(const point_t *path, size_t count, const point_t centers[26], int key_width,
                       uint32_t min_span)
{
    int left = MAX_COORDINATE, top = MAX_COORDINATE, right = 0, bottom = 0;
    for (unsigned i = 0; i < 26; ++i) {
        const point_t p = centers[i];
        if (p.x < 0 || p.x > MAX_COORDINATE || p.y < 0 || p.y > MAX_COORDINATE) return false;
        if (p.x < left) left = p.x;
        if (p.y < top) top = p.y;
        if (p.x > right) right = p.x;
        if (p.y > bottom) bottom = p.y;
    }
    bool moved = false;
    for (size_t i = 0; i < count; ++i) {
        const point_t p = path[i];
        if (p.x < left - key_width || p.x > right + key_width ||
            p.y < top - key_width || p.y > bottom + key_width) return false;
        if (squared_distance(path[0], p) >= min_span * min_span) moved = true;
    }
    return moved;
}

static size_t spelling_path(const char *spelling, const point_t centers[26],
                            point_t path[WORD_MAX_SPELLING])
{
    if (!spelling) return 0;
    size_t length = 0;
    for (; spelling[length]; ++length) {
        const char letter = spelling[length];
        if (length == WORD_MAX_SPELLING || letter < 'a' || letter > 'z') return 0;
        path[length] = centers[letter - 'a'];
    }
    return length;
}

typedef struct {
    point_t points[TRACE_SAMPLES];
    uint32_t length, width;
} swipe_path_t;

static int32_t bounded_offset(int32_t value, int32_t limit)
{
    if (value < -limit) return -limit;
    if (value > limit) return limit;
    return value;
}

static uint32_t compensate_offset(const swipe_path_t *actual, point_t ideal[TRACE_SAMPLES])
{
    int32_t dx = 0, dy = 0;
    for (unsigned i = 0; i < TRACE_SAMPLES; ++i) {
        dx += actual->points[i].x - ideal[i].x;
        dy += actual->points[i].y - ideal[i].y;
    }
    /* A thumb often crosses a keyboard row with a consistent bias. Fit a
     * small translation while still charging for its magnitude so nearby
     * keys retain meaning even when two paths have identical shapes. */
    const int32_t limit = (int32_t)actual->width / 3;
    dx = bounded_offset(dx * 2 / (3 * TRACE_SAMPLES), limit);
    dy = bounded_offset(dy * 2 / (3 * TRACE_SAMPLES), limit);
    for (unsigned i = 0; i < TRACE_SAMPLES; ++i) {
        ideal[i].x += dx;
        ideal[i].y += dy;
    }
    return (uint32_t)(4 * (dx * dx + dy * dy));
}

static uint32_t trim_endpoint(point_t *endpoint, point_t adjacent, point_t observed, uint32_t width)
{
    const int32_t dx = adjacent.x - endpoint->x, dy = adjacent.y - endpoint->y;
    const int32_t projection = (observed.x - endpoint->x) * dx + (observed.y - endpoint->y) * dy;
    if (projection <= 0) return 0;
    const uint32_t length = segment_length(*endpoint, adjacent);
    const uint32_t max_trim = length < width ? length / 3 : width / 3;
    const uint32_t projected = (uint32_t)projection / length;
    const int32_t trim = (int32_t)(projected < max_trim ? projected : max_trim);
    endpoint->x += dx * trim / (int32_t)length;
    endpoint->y += dy * trim / (int32_t)length;
    return (uint32_t)(trim * trim);
}

static uint32_t trim_template(point_t *path, size_t count, const swipe_path_t *actual)
{
    size_t start = 1, end = count - 1;
    while (start < count && !squared_distance(path[0], path[start])) ++start;
    if (start == count) return 0;
    while (end > 0 && !squared_distance(path[count - 1], path[end - 1])) --end;

    /* Contact begins and ends over a key area. Allow a short clipping of
     * the leading/trailing segment, without letting the fit skip a key. */
    point_t first = path[0], last = path[count - 1];
    const uint32_t start_trim = trim_endpoint(&first, path[start], actual->points[0], actual->width);
    const uint32_t end_trim = trim_endpoint(&last, path[end - 1], actual->points[TRACE_SAMPLES - 1], actual->width);
    for (size_t i = 0; i < start; ++i) path[i] = first;
    for (size_t i = end; i < count; ++i) path[i] = last;
    return start_trim + end_trim;
}

static void soften_turns(point_t *path, size_t count, uint32_t radius)
{
    point_t previous = path[0];
    for (size_t i = 1; i + 1 < count; ++i) {
        const point_t key = path[i], next = path[i + 1];
        const uint32_t before = segment_length(previous, key), after = segment_length(next, key);
        if (before && after) {
            const int32_t incoming = (int32_t)(radius < before / 3 ? radius : before / 3);
            const int32_t outgoing = (int32_t)(radius < after / 3 ? radius : after / 3);
            /* A smooth finger turn can pass inside the geometric corner. */
            path[i].x += ((previous.x - key.x) * incoming / (int32_t)before +
                          (next.x - key.x) * outgoing / (int32_t)after) / 3;
            path[i].y += ((previous.y - key.y) * incoming / (int32_t)before +
                          (next.y - key.y) * outgoing / (int32_t)after) / 3;
        }
        previous = key;
    }
}

static bool prepare_path(const point_t *points, size_t count, const point_t centers[26],
                         bool forgiving, swipe_path_t *path)
{
    if (!points || !centers || count < 2 || count > APP_PINYIN_SWIPE_MAX_POINTS) return false;
    const uint32_t key_width = segment_length(centers[0], centers['s' - 'a']);
    const uint32_t min_span = forgiving ? key_width / 2U : 2U * key_width / 3U;
    if (key_width < 12 || key_width > 128 || !spans_keys(points, count, centers, (int)key_width, min_span))
        return false;
    path->width = key_width;
    path->length = resample(points, count, path->points, TRACE_SAMPLES);
    return path->length >= (forgiving ? min_span : 3U * key_width / 4U);
}

static uint32_t score_word(const swipe_path_t *actual, const point_t centers[26],
                           const char *word, bool forgiving)
{
    if (!word || !word[0]) return UINT32_MAX;
    const unsigned first = (unsigned)(word[0] - 'a');
    if (first >= 26) return UINT32_MAX;
    const uint32_t width = actual->width, unit = width * width;
    const uint32_t endpoint_radius = (forgiving ? 3U * width / 2U : 5U * width / 4U);
    const uint32_t start = squared_distance(actual->points[0], centers[first]);
    if (start > endpoint_radius * endpoint_radius) return UINT32_MAX;
    const size_t length = strlen(word);
    if (length < 2 || length > WORD_MAX_SPELLING) return UINT32_MAX;
    const unsigned last = (unsigned)(word[length - 1] - 'a');
    if (last >= 26) return UINT32_MAX;
    const uint32_t end = squared_distance(actual->points[TRACE_SAMPLES - 1], centers[last]);
    if (end > endpoint_radius * endpoint_radius) return UINT32_MAX;

    point_t path[WORD_MAX_SPELLING], ideal[TRACE_SAMPLES];
    if (!spelling_path(word, centers, path)) return UINT32_MAX;
    const uint32_t clipping = forgiving ? trim_template(path, length, actual) : 0;
    if (forgiving && actual->length + width / 6U < path_length(path, length))
        soften_turns(path, length, width / 3U);
    const uint32_t ideal_length = resample(path, length, ideal, TRACE_SAMPLES);
    if (!ideal_length) return UINT32_MAX;
    const uint32_t delta = ideal_length > actual->length ? ideal_length - actual->length :
                           actual->length - ideal_length;
    /* A longer, real finger trace may pick up small wiggles at every key.
     * Keep the shape and endpoint checks, but budget the length error
     * against the ideal word length as well as the keyboard spacing. */
    const uint32_t length_limit = forgiving ? 6U * width + ideal_length / 5U : 8U * width;
    if (delta > length_limit) return UINT32_MAX;
    const uint32_t translation = forgiving ? compensate_offset(actual, ideal) : 0;
    const uint32_t endpoints = forgiving ? start + end +
        squared_distance(actual->points[0], ideal[0]) +
        squared_distance(actual->points[TRACE_SAMPLES - 1], ideal[TRACE_SAMPLES - 1]) :
        2U * (start + end);
    const uint32_t score = alignment_cost(actual->points, ideal) + endpoints + translation + clipping +
                           delta * delta / (forgiving ? 14U : 6U) +
                           (uint32_t)(length - 2) * unit / 12U;
    return score <= (forgiving ? 13U : 9U) * unit ? score : UINT32_MAX;
}

typedef struct {
    const char **words;
    uint32_t scores[APP_PINYIN_SWIPE_MAX_MATCHES];
    size_t count, capacity;
    uint32_t preferred_starts, start_bonus;
} swipe_ranking_t;

static uint32_t preferred_starts(const swipe_path_t *path, const point_t centers[26], uint32_t next_letters)
{
    if (!next_letters) return 0;
    const uint32_t unit = path->width * path->width;
    uint32_t distances[26];
    uint32_t nearest = UINT32_MAX;
    for (unsigned i = 0; i < 26; ++i) {
        distances[i] = squared_distance(path->points[0], centers[i]);
        if (distances[i] < nearest) nearest = distances[i];
    }
    if (nearest < unit / 9U) return 0;

    /* Keep context within 60% of a key pitch and close to the nearest key.
     * A preference needs both a plausible favored and unfavored start. */
    uint32_t nearby = 0;
    for (unsigned i = 0; i < 26; ++i) {
        if (distances[i] <= 9U * unit / 25U && distances[i] - nearest <= unit / 6U)
            nearby |= UINT32_C(1) << i;
    }
    return nearby & ~next_letters ? nearby & next_letters : 0;
}

static void rank_candidate(swipe_ranking_t *ranking, const char *word, uint32_t score)
{
    if (ranking->preferred_starts & (UINT32_C(1) << (word[0] - 'a')))
        score -= score < ranking->start_bonus ? score : ranking->start_bonus;
    size_t index = 0;
    for (; index < ranking->count && ranking->scores[index] <= score; ++index)
        if (!strcmp(ranking->words[index], word)) return;
    if (index == ranking->capacity) return;
    size_t end = index;
    while (end < ranking->count && strcmp(ranking->words[end], word)) ++end;
    if (end == ranking->count) {
        if (end < ranking->capacity) ++ranking->count;
        else --end;
    }
    for (; end > index; --end) {
        ranking->words[end] = ranking->words[end - 1];
        ranking->scores[end] = ranking->scores[end - 1];
    }
    ranking->words[index] = word;
    ranking->scores[index] = score;
}

static void rank_words(const swipe_path_t *path, const point_t centers[26], size_t word_count,
                       app_pinyin_swipe_word_at_t word_at, void *context, bool forgiving,
                       swipe_ranking_t *ranking)
{
    for (size_t i = 0; i < word_count; ++i) {
        const char *spelling = word_at(i, context);
        const uint32_t score = score_word(path, centers, spelling, forgiving);
        if (score != UINT32_MAX) rank_candidate(ranking, spelling, score);
    }
}

size_t app_pinyin_swipe_match(const point_t *points, size_t point_count, const point_t centers[26],
                             size_t word_count, app_pinyin_swipe_word_at_t word_at,
                             void *context, const char **matches, size_t match_capacity)
{
    swipe_path_t path;
    if (!word_at || !matches || !match_capacity || word_count > MAX_WORDS ||
        !prepare_path(points, point_count, centers, false, &path)) return 0;
    if (match_capacity > APP_PINYIN_SWIPE_MAX_MATCHES) match_capacity = APP_PINYIN_SWIPE_MAX_MATCHES;
    swipe_ranking_t ranking = {.words = matches, .capacity = match_capacity};
    rank_words(&path, centers, word_count, word_at, context, false, &ranking);
    size_t count = ranking.count;
    const uint32_t ambiguity = 3U * path.width * path.width;
    while (count > 1 && ranking.scores[count - 1] - ranking.scores[0] > ambiguity) --count;
    return count;
}

typedef struct {
    const char *spelling;
    uint32_t cost;
    uint8_t previous, length, units;
} swipe_chain_t;

typedef struct {
    uint32_t distances[APP_PINYIN_SWIPE_MAX_POINTS];
    swipe_chain_t chains[APP_PINYIN_SWIPE_MAX_POINTS];
} chain_workspace_t;

static bool near_turn(const point_t *path, size_t at, size_t count, point_t key, uint32_t radius)
{
    const uint32_t distance = squared_distance(path[at], key);
    return distance <= radius * radius &&
           (!at || at + 1 == count ||
            (distance < squared_distance(path[at - 1], key) &&
             distance <= squared_distance(path[at + 1], key)));
}

/* Joining the end of one spelling to the start of the next should generally
 * look like one line, not a way to discard an unmatched section of the trace. */
static uint32_t bridge_cost(const point_t *path, const uint32_t *distances, size_t from, size_t to,
                            point_t last_key, point_t first_key, uint32_t width)
{
    const uint32_t traveled = distances[to] - distances[from];
    const uint32_t direct = segment_length(path[from], path[to]);
    const uint32_t detour = traveled > direct ? traveled - direct : 0;
    if (traveled > 11U * width || detour > width / 2U) return UINT32_MAX;
    const uint32_t expected = segment_length(last_key, first_key);
    const uint32_t delta = traveled > expected ? traveled - expected : expected - traveled;
    if (delta > width) return UINT32_MAX;
    return traveled * (width / 12U + detour * detour / width) + delta * delta / 4U;
}

typedef struct {
    uint32_t cost;
    uint8_t previous;
    bool computed;
} chain_prefix_t;

static chain_prefix_t best_prefix(const point_t *path, const uint32_t *distances, size_t start,
                                  const point_t centers[26], point_t first_key, uint32_t width,
                                  const swipe_chain_t *chains)
{
    chain_prefix_t prefix = {.cost = start ? UINT32_MAX : 0, .computed = true};
    for (size_t end = 1; end <= start; ++end) {
        if (!chains[end].units || chains[end].units >= CHAIN_UNITS) continue;
        const char *last = chains[end].spelling;
        const point_t last_key = centers[last[strlen(last) - 1] - 'a'];
        const uint32_t bridge = bridge_cost(path, distances, end, start, last_key, first_key, width);
        if (bridge != UINT32_MAX && chains[end].cost + bridge < prefix.cost) {
            prefix.previous = (uint8_t)end;
            prefix.cost = chains[end].cost + bridge;
        }
    }
    return prefix;
}

static uint32_t local_fit(const point_t *path, size_t count, const point_t *ideal,
                          uint32_t start_error, uint32_t end_error, uint32_t length_delta)
{
    point_t observed[TRACE_SAMPLES];
    if (!resample(path, count, observed, TRACE_SAMPLES)) return UINT32_MAX;
    return alignment_cost(observed, ideal) / TRACE_SAMPLES +
           (start_error + end_error) / 6U + length_delta * length_delta / 10U;
}

static uint32_t supported_letter_cost(const point_t *letters, size_t length, const point_t *trace,
                                      size_t count, uint32_t width)
{
    const uint32_t unit = width * width;
    const uint32_t regular = (uint32_t)length * unit / 4U;
    if (length != 3 || count < 3) return regular;

    const point_t a = letters[0], b = letters[1], c = letters[2];
    const int32_t x = c.x - a.x, y = c.y - a.y;
    const int32_t bx = b.x - a.x, by = b.y - a.y;
    const int32_t along = x * bx + y * by;
    const int32_t outline = x * by - y * bx;
    const int32_t ideal_length = (int32_t)segment_length(a, c);
    if (!ideal_length || along <= 0 || along >= x * x + y * y) return regular;
    const uint32_t deviation = (uint32_t)abs(outline) / (uint32_t)ideal_length;
    if (deviation < 2 || deviation * 4U > width) return regular;

    const point_t start = trace[0], end = trace[count - 1];
    const int32_t dx = end.x - start.x, dy = end.y - start.y;
    const int32_t observed_length = (int32_t)segment_length(start, end);
    if (!observed_length) return regular;
    const int32_t target = (along / ideal_length) * observed_length / ideal_length;
    const int32_t tolerance = (int32_t)width / 2;
    const int32_t threshold = (int32_t)((deviation + 1U) / 2U) * observed_length;
    /* Shallow bends carry less positional error than a full-key penalty.
     * Discount the intermediate key when the ordered samples bend toward it
     * at the expected point in the path. Relative chords cancel thumb bias. */
    for (size_t i = 1; i + 1 < count; ++i) {
        const int32_t px = trace[i].x - start.x, py = trace[i].y - start.y;
        const int32_t progress = (dx * px + dy * py) / observed_length;
        if (abs(progress - target) > tolerance) continue;
        const int32_t lateral = dx * py - dy * px;
        if ((outline < 0 ? -lateral : lateral) >= threshold)
            return unit / 2U + width / 16U + deviation * deviation / 4U;
    }
    return regular;
}

static void match_units(const point_t *points, const uint32_t *distances, size_t count, size_t start,
                         const point_t centers[26], uint32_t width, swipe_chain_t *chains,
                         unsigned *budget)
{
    const uint32_t unit = width * width;
    const uint32_t radius = 4U * width / 5U;
    chain_prefix_t prefixes[26] = {0};
    const size_t syllables = app_pinyin_syllable_count(), entries = app_pinyin_spelling_count();
    for (size_t i = 0; i < entries && *budget; ++i) {
        const char *spelling = app_pinyin_spelling_at(i);
        const size_t first = (size_t)(spelling[0] - 'a');
        const point_t first_key = centers[first];
        if (!near_turn(points, start, count, first_key, radius)) continue;
        if (!prefixes[first].computed)
            prefixes[first] = best_prefix(points, distances, start, centers, first_key, width, chains);
        const chain_prefix_t prefix = prefixes[first];
        if (prefix.cost == UINT32_MAX) continue;
        const size_t length = strlen(spelling);
        const size_t full_length = chains[prefix.previous].length + length;
        if (length < 2 || full_length > WORD_MAX_SPELLING) continue;
        point_t letters[WORD_MAX_SPELLING], ideal[TRACE_SAMPLES];
        if (spelling_path(spelling, centers, letters) != length) continue;
        const uint32_t ideal_length = resample(letters, length, ideal, TRACE_SAMPLES);
        if (!ideal_length) continue;
        const point_t last_key = letters[length - 1];
        const uint32_t length_tolerance = width + ideal_length / 4U;
        for (size_t end = start + 1; end < count && *budget; ++end) {
            const uint32_t traveled = distances[end] - distances[start];
            if (traveled + length_tolerance < ideal_length) continue;
            if (traveled > ideal_length + length_tolerance) break;
            if (!near_turn(points, end, count, last_key, radius)) continue;
            const uint32_t delta = traveled > ideal_length ? traveled - ideal_length :
                                   ideal_length - traveled;
            --*budget;
            const uint32_t fit = local_fit(points + start, end - start + 1, ideal,
                                           squared_distance(points[start], first_key),
                                           squared_distance(points[end], last_key), delta);
            if (fit > unit * 3U / 5U) continue;
            /* Favor complete familiar words, which can disambiguate letters
             * whose keys otherwise lie directly between other keys. */
            const uint32_t cost = prefix.cost + fit * traveled / width +
                (i < syllables ? 3U * unit / 4U : unit / 2U) +
                supported_letter_cost(letters, length, points + start, end - start + 1, width);
            if (!chains[end].units || cost < chains[end].cost)
                chains[end] = (swipe_chain_t){spelling, cost, prefix.previous,
                    (uint8_t)full_length, (uint8_t)(chains[prefix.previous].units + 1)};
        }
    }
}

static bool decode_chain(const point_t *points, size_t point_count, const point_t centers[26],
                         const swipe_path_t *path, char result[WORD_MAX_SPELLING + 1])
{
    /* Keep the larger search table off the display task's stack. If it cannot
     * be allocated, recognition still has the complete fixed word list. */
    chain_workspace_t *workspace = calloc(1, sizeof(*workspace));
    if (!workspace) return false;
    uint32_t *distances = workspace->distances;
    swipe_chain_t *chains = workspace->chains;
    for (size_t i = 1; i < point_count; ++i)
        distances[i] = distances[i - 1] + segment_length(points[i - 1], points[i]);
    unsigned budget = CHAIN_FIT_LIMIT;
    for (size_t start = 0; start + 1 < point_count && budget; ++start)
        match_units(points, distances, point_count, start, centers, path->width, chains, &budget);
    size_t end = point_count - 1;
    const bool found = chains[end].units >= 2;
    if (found) {
        size_t offset = chains[end].length;
        result[offset] = '\0';
        while (end) {
            const size_t length = strlen(chains[end].spelling);
            offset -= length;
            memcpy(result + offset, chains[end].spelling, length);
            end = chains[end].previous;
        }
    }
    free(workspace);
    return found;
}

typedef struct {
    const char *word;
    uint32_t cost, hash;
    uint16_t previous;
    uint8_t length, units;
} sentence_state_t;

typedef struct {
    uint32_t cost;
    uint16_t state;
} sentence_prefix_t;

typedef struct {
    uint32_t distances[APP_PINYIN_SWIPE_MAX_POINTS], turns[APP_PINYIN_SWIPE_MAX_POINTS];
    sentence_state_t states[APP_PINYIN_SWIPE_MAX_POINTS * SENTENCE_ALTERNATIVES];
    sentence_prefix_t prefixes[26][SENTENCE_ALTERNATIVES];
    uint8_t prefix_count[26];
    point_t adjusted_points[APP_PINYIN_SWIPE_MAX_POINTS];
} sentence_workspace_t;

/* Compare a shared hash/length directly before merging spellings that were
 * reached with different word boundaries. Follow each path backward so no
 * complete sentence needs to live in the working table. */
static bool same_sentence(const sentence_state_t *states, sentence_state_t a, sentence_state_t b)
{
    if (a.hash != b.hash || a.length != b.length) return false;
    size_t na = a.word ? strlen(a.word) : 0, nb = b.word ? strlen(b.word) : 0;
    while (na && nb) {
        const size_t bytes = na < nb ? na : nb;
        na -= bytes;
        nb -= bytes;
        if (memcmp(a.word + na, b.word + nb, bytes)) return false;
        if (!na && a.previous) {
            a = states[a.previous];
            na = strlen(a.word);
        }
        if (!nb && b.previous) {
            b = states[b.previous];
            nb = strlen(b.word);
        }
    }
    return !na && !nb;
}

static void keep_sentence_state(sentence_state_t *states, size_t endpoint, sentence_state_t candidate)
{
    sentence_state_t *beam = states + endpoint * SENTENCE_ALTERNATIVES;
    size_t start = 0, duplicate = SENTENCE_ALTERNATIVES;
    while (start < SENTENCE_ALTERNATIVES && beam[start].units && beam[start].cost <= candidate.cost) {
        if (same_sentence(states, beam[start], candidate)) return;
        ++start;
    }
    if (start == SENTENCE_ALTERNATIVES) return;
    for (size_t i = start; i < SENTENCE_ALTERNATIVES && beam[i].units; ++i)
        if (same_sentence(states, beam[i], candidate)) { duplicate = i; break; }
    for (size_t i = duplicate < SENTENCE_ALTERNATIVES ? duplicate : SENTENCE_ALTERNATIVES - 1;
         i > start; --i) beam[i] = beam[i - 1];
    beam[start] = candidate;
}

static void keep_sentence_prefix(sentence_prefix_t *beam, size_t *count,
                                 const sentence_state_t *states, sentence_prefix_t candidate)
{
    size_t first = 0;
    for (; first < *count && beam[first].cost <= candidate.cost; ++first)
        if (same_sentence(states, states[beam[first].state], states[candidate.state])) return;
    if (first == SENTENCE_ALTERNATIVES) return;
    size_t end = first;
    while (end < *count && !same_sentence(states, states[beam[end].state], states[candidate.state])) ++end;
    if (end == *count) {
        if (end < SENTENCE_ALTERNATIVES) ++*count;
        else --end;
    }
    for (; end > first; --end) beam[end] = beam[end - 1];
    beam[first] = candidate;
}

static uint8_t sentence_prefixes(sentence_workspace_t *workspace, const point_t *points, size_t start,
                                 const point_t centers[26], size_t first, uint32_t width)
{
    sentence_prefix_t *prefixes = workspace->prefixes[first];
    if (!start) {
        prefixes[0] = (sentence_prefix_t){0};
        return 1;
    }
    size_t count = 0;
    const uint32_t *distances = workspace->distances;
    /* A bridge can span the expected move between two letters, but cannot
     * jump across an arbitrary unrecognized section of the gesture. */
    for (size_t end = start; end && distances[start] - distances[end] <= 11U * width; --end) {
        const size_t base = end * SENTENCE_ALTERNATIVES;
        for (size_t i = 0; i < SENTENCE_ALTERNATIVES; ++i) {
            const sentence_state_t state = workspace->states[base + i];
            if (!state.units) break;
            if (state.units >= SENTENCE_CHAIN_UNITS) continue;
            const char last = state.word[strlen(state.word) - 1];
            const point_t from = centers[last - 'a'], to = centers[first];
            const uint32_t bridge = bridge_cost(points, distances, end, start, from, to, width);
            if (bridge == UINT32_MAX) continue;

            /* Score the spatial route between words, too. Weight the samples
             * by distance so reporting more points around a key adds no bias. */
            const int32_t traveled = (int32_t)(distances[start] - distances[end]);
            uint32_t shape = 0;
            for (size_t at = end + 1; traveled && at < start; ++at) {
                const int32_t offset = (int32_t)(distances[at] - distances[end]);
                const point_t ideal = {
                    from.x + (to.x - from.x) * offset / traveled,
                    from.y + (to.y - from.y) * offset / traveled,
                };
                shape += squared_distance(points[at], ideal) * (distances[at + 1] - distances[at - 1]);
            }
            keep_sentence_prefix(prefixes, &count, workspace->states,
                (sentence_prefix_t){state.cost + bridge + shape / (8U * width), (uint16_t)(base + i)});
        }
    }
    return (uint8_t)count;
}

static uint32_t sentence_unit_fit(const point_t *points, size_t count, const point_t *ideal,
                                  uint32_t scale, uint32_t endpoint_error, uint32_t length_delta)
{
    /* Match all the arc-length samples in order. Endpoint proximity only
     * proposes boundaries. The fit still includes the full path between
     * them, with a small translation for thumb posture. */
    swipe_path_t interval = {.width = scale};
    if (!resample(points, count, interval.points, TRACE_SAMPLES)) return UINT32_MAX;
    point_t shifted[TRACE_SAMPLES];
    memcpy(shifted, ideal, sizeof(shifted));
    const uint32_t translation = compensate_offset(&interval, shifted);
    return (alignment_cost(interval.points, shifted) + translation) / TRACE_SAMPLES +
           endpoint_error / 6U + length_delta * length_delta / 10U;
}

static void match_sentence_units(sentence_workspace_t *workspace, const point_t *points, size_t count,
                                 size_t start, const point_t centers[26], uint32_t width,
                                 bool corrected, unsigned *budget)
{
    const uint32_t unit = width * width;
    const uint32_t *distances = workspace->distances;
    const size_t syllables = app_pinyin_syllable_count(), entries = app_pinyin_spelling_count();
    uint32_t prepared = 0;
    for (size_t i = 0; i < entries && *budget; ++i) {
        const char *spelling = app_pinyin_spelling_at(i);
        const size_t first = (size_t)(spelling[0] - 'a');
        const uint32_t bit = UINT32_C(1) << first;
        if (!(workspace->turns[start] & bit)) continue;
        if (!(prepared & bit)) {
            prepared |= bit;
            workspace->prefix_count[first] = sentence_prefixes(workspace, points, start, centers, first, width);
        }
        const size_t prefixes = workspace->prefix_count[first];
        if (!prefixes) continue;
        point_t letters[WORD_MAX_SPELLING], ideal[TRACE_SAMPLES], rounded[TRACE_SAMPLES];
        const size_t length = spelling_path(spelling, centers, letters);
        if (length < 2) continue;
        const uint32_t ideal_length = resample(letters, length, ideal, TRACE_SAMPLES);
        if (!ideal_length) continue;
        const uint32_t last_bit = UINT32_C(1) << (spelling[length - 1] - 'a');
        const uint32_t tolerance = width + ideal_length / 4U;
        const point_t unrounded[] = {letters[0], letters[length / 2], letters[length - 1]};
        uint32_t rounded_length = 0;
        for (size_t end = start + 1; end < count && *budget; ++end) {
            const uint32_t traveled = distances[end] - distances[start];
            if (traveled > ideal_length + tolerance) break;
            if (traveled + tolerance < ideal_length || !(workspace->turns[end] & last_bit)) continue;
            const point_t *shape = ideal;
            uint32_t expected_length = ideal_length;
            /* A thumb turning inside the keys makes a shorter curve. Select
             * one template per fit so the alignment budget stays bounded. */
            if (corrected && length > 2 && traveled + width / 4U < ideal_length) {
                if (!rounded_length) {
                    soften_turns(letters, length, width * 2U / 3U);
                    rounded_length = resample(letters, length, rounded, TRACE_SAMPLES);
                }
                if (rounded_length) {
                    shape = rounded;
                    expected_length = rounded_length;
                }
            }
            const uint32_t delta = traveled > expected_length ? traveled - expected_length : expected_length - traveled;
            --*budget;
            const uint32_t endpoint_error = squared_distance(points[start], letters[0]) +
                                            squared_distance(points[end], letters[length - 1]);
            /* The correction is an estimate. Give endpoint positions less
             * influence when the complete shape supports the candidate. */
            const uint32_t fit = sentence_unit_fit(points + start, end - start + 1, shape,
                                                    width, corrected ? endpoint_error / 2U : endpoint_error, delta);
            if (fit > unit * 7U / 10U) continue;
            const uint32_t stop_cost = fit * traveled / width + (i < syllables ? 3U * unit / 4U : unit / 2U) +
                supported_letter_cost(unrounded, length, points + start, end - start + 1, width);
            for (size_t j = 0; j < prefixes; ++j) {
                const sentence_prefix_t prefix = workspace->prefixes[first][j];
                const sentence_state_t previous = workspace->states[prefix.state];
                if (previous.length + length > APP_PINYIN_SWIPE_MAX_SPELLING) continue;
                uint32_t hash = previous.hash;
                for (size_t letter = 0; letter < length; ++letter)
                    hash = (hash ^ (uint8_t)spelling[letter]) * UINT32_C(16777619);
                keep_sentence_state(workspace->states, end,
                    (sentence_state_t){spelling, prefix.cost + stop_cost, hash, prefix.state,
                        (uint8_t)(previous.length + length), (uint8_t)(previous.units + 1)});
            }
        }
    }
}

static point_t nearest_key_offset(point_t point, const point_t centers[26])
{
    point_t key = centers[0];
    uint32_t nearest = squared_distance(point, key);
    for (size_t i = 1; i < 26; ++i) {
        const uint32_t distance = squared_distance(point, centers[i]);
        if (distance >= nearest) continue;
        nearest = distance;
        key = centers[i];
    }
    return (point_t){point.x - key.x, point.y - key.y};
}

static const point_t *correct_sentence_trace(point_t *adjusted, const point_t *observed, size_t count,
                                              const point_t centers[26], uint32_t width)
{
    const point_t start = nearest_key_offset(observed[0], centers);
    const point_t end = nearest_key_offset(observed[count - 1], centers);
    const uint32_t unit = width * width;
    /* Contact and lift-off can reveal a persistent thumb offset. Correct the
     * entire trajectory by the same vector when the two ends agree. Small or
     * conflicting offsets leave the existing shape model in use. */
    if (squared_distance(start, end) > unit / 2U) return observed;
    const int32_t limit = (int32_t)width / 2;
    const int32_t x = bounded_offset((start.x + end.x) * 2 / 5, limit);
    const int32_t y = bounded_offset((start.y + end.y) * 2 / 5, limit);
    if ((uint32_t)(x * x + y * y) < unit / 100U) return observed;
    for (size_t i = 0; i < count; ++i)
        adjusted[i] = (point_t){observed[i].x - x, observed[i].y - y};
    return adjusted;
}

static size_t match_sentence_trace(sentence_workspace_t *workspace, const point_t *points,
                                   size_t point_count, const point_t centers[26], uint32_t width,
                                   bool corrected, unsigned *budget,
                                   char matches[][APP_PINYIN_SWIPE_MAX_SPELLING + 1], size_t match_capacity)
{
    const uint32_t radius = corrected ? width : 4U * width / 5U;
    for (size_t i = 0; i < point_count; ++i) {
        if (i) workspace->distances[i] = workspace->distances[i - 1] + segment_length(points[i - 1], points[i]);
        for (unsigned letter = 0; letter < 26; ++letter)
            if (near_turn(points, i, point_count, centers[letter], radius))
                workspace->turns[i] |= UINT32_C(1) << letter;
    }
    for (size_t start = 0; start + 1 < point_count && *budget; ++start)
        if (workspace->turns[start])
            match_sentence_units(workspace, points, point_count, start, centers, width, corrected, budget);
    const size_t last = (point_count - 1) * SENTENCE_ALTERNATIVES;
    size_t copied = 0;
    for (size_t i = 0; i < SENTENCE_ALTERNATIVES && copied < match_capacity; ++i) {
        sentence_state_t state = workspace->states[last + i];
        if (state.units < 2) continue;
        size_t offset = state.length;
        matches[copied][offset] = '\0';
        while (state.units) {
            const size_t length = strlen(state.word);
            offset -= length;
            memcpy(matches[copied] + offset, state.word, length);
            state = workspace->states[state.previous];
        }
        ++copied;
    }
    return copied;
}

static size_t decode_sentence(const point_t *observed, size_t point_count, const point_t centers[26],
                              const swipe_path_t *path, char matches[][APP_PINYIN_SWIPE_MAX_SPELLING + 1],
                              size_t match_capacity)
{
    sentence_workspace_t *workspace = calloc(1, sizeof(*workspace));
    if (!workspace) return 0;
    const point_t *points = correct_sentence_trace(workspace->adjusted_points, observed,
                                                   point_count, centers, path->width);
    const bool corrected = points != observed;
    unsigned budget = SENTENCE_FIT_LIMIT;
    size_t copied = match_sentence_trace(workspace, points, point_count, centers, path->width,
                                         corrected, &budget, matches, match_capacity);
    if (!copied && corrected && budget) {
        /* A large offset may mean an adjacent key. Reuse the workspace and
         * the remaining fit budget for the original gesture in that case. */
        memset(workspace, 0, sizeof(*workspace));
        copied = match_sentence_trace(workspace, observed, point_count, centers, path->width,
                                      false, &budget, matches, match_capacity);
    }
    free(workspace);
    return copied;
}

static const char *dictionary_word(size_t index, void *context)
{
    (void)context;
    return app_pinyin_spelling_at(index);
}

static size_t common_first_letter(char letter, size_t low, size_t high)
{
    while (low < high) {
        const size_t middle = low + (high - low) / 2;
        if (app_pinyin_common_phrase_spelling_at(middle)[0] < letter) low = middle + 1;
        else high = middle;
    }
    return low;
}

static void rank_common_words(const swipe_path_t *path, const point_t centers[26],
                              swipe_ranking_t *ranking, bool extended)
{
    const uint32_t broad_radius = extended ? path->width : 3U * path->width / 2U;
    const uint32_t unit = path->width * path->width;
    const size_t count = extended ? app_pinyin_extended_count() : app_pinyin_common_phrase_count();
    if (!count) return;
    size_t low = 0;
    unsigned fitted = 0;
    uint32_t end_keys = 0;
    for (unsigned letter = 0; extended && letter < 26; ++letter)
        if (squared_distance(path->points[TRACE_SAMPLES - 1], centers[letter]) <= unit)
            end_keys |= UINT32_C(1) << letter;
    /* The packed vocabulary is sorted. Skip whole first-letter ranges away
     * from the touch-down position instead of visiting every dictionary row. */
    for (unsigned letter = 0; letter < 26; ++letter) {
        if (squared_distance(path->points[0], centers[letter]) > broad_radius * broad_radius) continue;
        const char start_key = (char)('a' + letter), next_key = start_key + 1;
        const size_t first = extended ? app_pinyin_extended_lower_bound(&start_key, 1, low) :
            common_first_letter(start_key, low, count);
        low = extended ? app_pinyin_extended_lower_bound(&next_key, 1, first) :
            common_first_letter(next_key, first, count);
        for (size_t i = first; i < low; ++i) {
            const char *spelling = extended ? app_pinyin_extended_key(i) : app_pinyin_common_phrase_spelling_at(i);
            if (extended) {
                const size_t length = strlen(spelling);
                if (!(end_keys & (UINT32_C(1) << (spelling[length - 1] - 'a')))) continue;
                if (++fitted > 6000) return;
            }
            const uint32_t fit = score_word(path, centers, spelling, true);
            if (fit == UINT32_MAX) continue;
            /* Word frequency resolves near ties. The entire frequency range
             * costs a fraction of a single key-width spatial error. */
            const unsigned priority = extended ? app_pinyin_extended_priority(i) :
                app_pinyin_common_phrase_priority(i);
            const uint32_t prior = (20U - priority) * unit / 60U + (extended ? unit / 4U : 0);
            rank_candidate(ranking, spelling, fit + prior);
        }
    }
}

size_t app_pinyin_swipe_decode_context(const point_t *points, size_t point_count, const point_t centers[26],
                                      uint32_t next_letters,
                                      char matches[][APP_PINYIN_SWIPE_MAX_SPELLING + 1], size_t match_capacity)
{
    swipe_path_t path;
    if (!matches || !match_capacity || !prepare_path(points, point_count, centers, true, &path)) return 0;
    if (match_capacity > APP_PINYIN_SWIPE_MAX_MATCHES) match_capacity = APP_PINYIN_SWIPE_MAX_MATCHES;
    const char *known[APP_PINYIN_SWIPE_MAX_MATCHES];
    const uint32_t unit = path.width * path.width;
    swipe_ranking_t ranking = {
        .words = known, .capacity = match_capacity,
        .preferred_starts = preferred_starts(&path, centers, next_letters), .start_bonus = unit / 4U,
    };
    rank_words(&path, centers, app_pinyin_spelling_count(), dictionary_word, NULL, true, &ranking);
    const bool needs_syllables = !ranking.count || ranking.scores[0] > 2U * unit;
    rank_common_words(&path, centers, &ranking, false);
    const bool has_common_word = ranking.count != 0;
    const bool sentence = path.length >= SENTENCE_ARC_KEY_WIDTHS * path.width;
    if (!sentence && (!ranking.count || ranking.scores[0] >
        unit / 2U + (strlen(ranking.words[0]) - 2U) * unit / 12U))
        rank_common_words(&path, centers, &ranking, true);
    const size_t found = ranking.count;
    const bool close_cold_word = found && ranking.scores[0] <=
        (strlen(ranking.words[0]) - 2U) * unit / 12U + 3U * unit / 2U;
    /* A five-unit chain can run out of segments well before the sentence
     * cutoff. In that gap, try local decoding when the built-in dictionary
     * finds no match and the wider dictionary only has a weak fit. */
    const bool try_composition = !has_common_word && !close_cold_word &&
        path.length >= SENTENCE_FALLBACK_KEY_WIDTHS * path.width;
    if (try_composition || (sentence && found && ranking.scores[0] > 4U * unit)) {
        const size_t sentences = decode_sentence(points, point_count, centers, &path, matches, match_capacity);
        if (sentences) return sentences;
    }
    char generated[WORD_MAX_SPELLING + 1];
    uint32_t generated_score = UINT32_MAX;
    /* Most known strokes never need the syllable search. The additional
     * spelling must make sense as one complete gesture, not just each cut. */
    if (path.length >= 4U * path.width && needs_syllables &&
        (!found || ranking.scores[0] > unit / 2U) &&
        decode_chain(points, point_count, centers, &path, generated)) {
        generated_score = score_word(&path, centers, generated, true);
        if (generated_score != UINT32_MAX) generated_score += unit / 2U;
        for (size_t i = 0; i < found; ++i)
            if (!strcmp(known[i], generated)) generated_score = UINT32_MAX;
    }

    const uint32_t best = found && ranking.scores[0] < generated_score ? ranking.scores[0] : generated_score;
    size_t next = 0, copied = 0;
    for (; copied < match_capacity; ++copied) {
        const uint32_t known_score = next < found ? ranking.scores[next] : UINT32_MAX;
        const uint32_t score = known_score < generated_score ? known_score : generated_score;
        if (score == UINT32_MAX || score - best > 5U * unit) break;
        if (known_score <= generated_score) strcpy(matches[copied], known[next++]);
        else {
            strcpy(matches[copied], generated);
            generated_score = UINT32_MAX;
        }
    }
    return copied;
}

size_t app_pinyin_swipe_decode(const point_t *points, size_t point_count, const point_t centers[26],
                              char matches[][APP_PINYIN_SWIPE_MAX_SPELLING + 1], size_t match_capacity)
{
    return app_pinyin_swipe_decode_context(points, point_count, centers, 0, matches, match_capacity);
}
