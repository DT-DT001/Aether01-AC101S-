#include "app_pinyin.h"
#include "app_pinyin_swipe.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef app_pinyin_swipe_point_t point_t;

static point_t centers[26];

static void layout(void)
{
    const char *rows[] = {"qwertyuiop", "asdfghjkl", "zxcvbnm"};
    const int offsets[] = {42, 64, 108};
    for (unsigned row = 0; row < 3; ++row)
        for (size_t col = 0; rows[row][col]; ++col)
            centers[rows[row][col] - 'a'] = (point_t){offsets[row] + (int)col * 44, 290 + (int)row * 47};
}

static const char *word_at(size_t index, void *context)
{
    return ((const char *const *)context)[index];
}

static const char *pinyin_word_at(size_t index, void *context)
{
    (void)context;
    return app_pinyin_spelling_at(index);
}

static size_t draw(point_t points[APP_PINYIN_SWIPE_MAX_POINTS], const char *word, int noise)
{
    size_t count = 1;
    points[0] = centers[word[0] - 'a'];
    for (size_t key = 1; word[key]; ++key) {
        const point_t a = centers[word[key - 1] - 'a'], b = centers[word[key] - 'a'];
        for (int step = 1; step <= 3; ++step) {
            const int jitter = step & 1 ? noise : -noise;
            assert(count < APP_PINYIN_SWIPE_MAX_POINTS);
            points[count++] = (point_t){
                a.x + (b.x - a.x) * step / 3 + jitter,
                a.y + (b.y - a.y) * step / 3 - jitter,
            };
        }
    }
    return count;
}

static void test_phrases(void)
{
    static const char *dictionary[] = {
        "ni", "nei", "na", "nen", "hao", "ha", "ninhao", "nihao", "niyao", "zhong", "zhongguo", "zhiguo", "zhuogu", "guo",
        "kaishi", "jintian", "mingtian", "beijing", "shanghai", "zaijian", "xiexie", "meiguanxi", "baoqian",
    };
    point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
    const char *matches[APP_PINYIN_SWIPE_MAX_MATCHES];
    const char *phrases[] = {"nihao", "zhongguo", "xiexie", "meiguanxi", "mingtian", "zaijian"};
    for (size_t i = 0; i < sizeof(phrases) / sizeof(phrases[0]); ++i) {
        const size_t count = draw(points, phrases[i], 7);
        const size_t results = app_pinyin_swipe_match(points, count, centers,
            sizeof(dictionary) / sizeof(dictionary[0]), word_at, dictionary, matches, APP_PINYIN_SWIPE_MAX_MATCHES);
        assert(results && !strcmp(matches[0], phrases[i]));
    }
    puts("PASS curved phrases, a repeated letter, crossed keys and off-center samples");
}

static void test_disambiguation(void)
{
    static const char *dictionary[] = {"rui", "ri", "n", "nei", "ni", "nin", "ri", "not-spelling", "unrecognizedoverlongstringexample"};
    const char *matches[APP_PINYIN_SWIPE_MAX_MATCHES];
    const point_t fast[] = {centers['n' - 'a'], centers['i' - 'a']};
    size_t results = app_pinyin_swipe_match(fast, 2, centers, sizeof(dictionary) / sizeof(dictionary[0]),
        word_at, dictionary, matches, APP_PINYIN_SWIPE_MAX_MATCHES);
    assert(results && !strcmp(matches[0], "ni"));

    const point_t straight[] = {centers['r' - 'a'], centers['u' - 'a'], centers['i' - 'a']};
    results = app_pinyin_swipe_match(straight, 3, centers, sizeof(dictionary) / sizeof(dictionary[0]),
        word_at, dictionary, matches, APP_PINYIN_SWIPE_MAX_MATCHES);
    assert(results == 2 && !strcmp(matches[0], "ri") && !strcmp(matches[1], "rui"));

    const point_t wrong_end[] = {centers['n' - 'a'], centers['q' - 'a']};
    assert(!app_pinyin_swipe_match(wrong_end, 2, centers, sizeof(dictionary) / sizeof(dictionary[0]),
        word_at, dictionary, matches, APP_PINYIN_SWIPE_MAX_MATCHES));
    puts("PASS two-frame swipes, correct endpoint, unique ranked candidates and ambiguous collinear keys");
}

static void test_limits(void)
{
    static const char *dictionary[] = {"ni", "lianxizhongwenshurufa"};
    point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
    const char *match = NULL;
    const size_t long_trace = draw(points, dictionary[1], 4);
    assert(app_pinyin_swipe_match(points, long_trace, centers, 2, word_at, dictionary, &match, 1) == 1);
    assert(!strcmp(match, dictionary[1]));
    for (size_t i = 0; i < APP_PINYIN_SWIPE_MAX_POINTS; ++i) {
        points[i] = centers['n' - 'a'];
        points[i].x += (int)(i % 6U) - 3;
        points[i].y += (int)(i % 5U) - 2;
    }
    assert(!app_pinyin_swipe_match(points, APP_PINYIN_SWIPE_MAX_POINTS, centers, 2, word_at, dictionary, &match, 1));
    assert(!app_pinyin_swipe_match(points, APP_PINYIN_SWIPE_MAX_POINTS + 1, centers, 2, word_at, dictionary, &match, 1));
    points[1] = (point_t){200, 100};
    points[2] = centers['i' - 'a'];
    assert(!app_pinyin_swipe_match(points, 3, centers, 2, word_at, dictionary, &match, 1));
    puts("PASS bounded long trace, rejection of stationary jitter, oversized input and leaving the keyboard");
}

static void test_dictionary(void)
{
    static const struct { const char *spelling; const char *text; } words[] = {
        {"nihao", "你好"}, {"zhongguo", "中国"}, {"xiexie", "谢谢"},
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
        const size_t count = draw(points, words[i].spelling, 7);
        const char *match = NULL;
        assert(app_pinyin_swipe_match(points, count, centers, app_pinyin_spelling_count(),
            pinyin_word_at, NULL, &match, 1) == 1);
        assert(!strcmp(match, words[i].spelling));
        app_pinyin_candidate_t candidate;
        assert(app_pinyin_lookup(match, 0, &candidate, 1, NULL) == 1);
        assert(!strcmp(candidate.text, words[i].text) && candidate.consumed == strlen(match));
    }
    puts("PASS noisy swipes match complete production vocabulary and offer the expected phrase");
}

static int offset_sample(uint32_t *state, unsigned radius)
{
    *state = *state * 1664525U + 1013904223U;
    return (int)((*state >> 16) % (2U * radius + 1U)) - (int)radius;
}

static void add_sample(point_t points[APP_PINYIN_SWIPE_MAX_POINTS], size_t *count, point_t point)
{
    if (*count) {
        const int dx = point.x - points[*count - 1].x;
        const int dy = point.y - points[*count - 1].y;
        if (dx * dx + dy * dy < 16) return;
    }
    if (*count == APP_PINYIN_SWIPE_MAX_POINTS) *count = app_pinyin_swipe_compact(points, *count);
    points[(*count)++] = point;
}

static size_t draw_uneven(point_t points[APP_PINYIN_SWIPE_MAX_POINTS], const char *word, uint32_t seed)
{
    uint32_t state = seed;
    point_t previous = {0};
    size_t count = 0;
    for (size_t i = 0; word[i]; ++i) {
        point_t target = centers[word[i] - 'a'];
        target.x += offset_sample(&state, seed ? 14 : 0);
        target.y += offset_sample(&state, seed ? 14 : 0);
        if (i) {
            const int frames = 3 + (int)((seed + i) % 4U);
            for (int step = 1; step < frames; ++step) {
                const point_t sample = {
                    .x = previous.x + (target.x - previous.x) * step / frames + offset_sample(&state, seed ? 5 : 0),
                    .y = previous.y + (target.y - previous.y) * step / frames + offset_sample(&state, seed ? 5 : 0),
                };
                add_sample(points, &count, sample);
            }
        }
        add_sample(points, &count, target);
        previous = target;
    }
    return count;
}

static void test_extended_swipes(void)
{
    static const struct {
        const char *spelling, *text;
        bool listed;
    } words[] = {
        {"kaishiluyin", "开始录音", true},
        {"xiazaitupian", "下载图片", true},
        {"dakaijishiben", "打开记事本", true},
        {"woqu", "我去", true},
        {"xiongmao", "熊猫", true},
        {"changjiang", "长江", true},
        {"woxianghecha", "我想喝茶", false},
    };
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        bool listed = false;
        for (size_t k = 0; k < app_pinyin_spelling_count(); ++k)
            if (!strcmp(app_pinyin_spelling_at(k), words[i].spelling)) listed = true;
        for (size_t k = 0; k < app_pinyin_common_phrase_count(); ++k)
            if (!strcmp(app_pinyin_common_phrase_spelling_at(k), words[i].spelling)) listed = true;
        assert(listed == words[i].listed);

        unsigned correct = 0;
        for (uint32_t trial = 0; trial <= 16; ++trial) {
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
            const size_t count = draw_uneven(points, words[i].spelling, 511U * trial);
            char matches[3][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            const size_t found = app_pinyin_swipe_decode(points, count, centers, matches, 3);
            for (size_t j = 0; j < found; ++j) {
                if (strcmp(matches[j], words[i].spelling)) continue;
                app_pinyin_candidate_t candidate;
                assert(app_pinyin_lookup(matches[j], 0, &candidate, 1, NULL) == 1);
                assert(!strcmp(candidate.text, words[i].text));
                assert(candidate.consumed == strlen(matches[j]));
                ++correct;
            }
            if (!trial) assert(found && !strcmp(matches[0], words[i].spelling));
        }
        assert(correct >= (words[i].listed ? 17 : 13));
    }
    const point_t near_stationary[] = {centers[0], {centers[0].x + 8, centers[0].y - 5}};
    const point_t outside[] = {centers[0], {200, 100}, centers['m' - 'a']};
    char match[1][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
    assert(app_pinyin_swipe_decode(near_stationary, 2, centers, match, 1) == 0);
    assert(app_pinyin_swipe_decode(outside, 3, centers, match, 1) == 0);
    puts("PASS expanded phrases and unlisted compositions with irregular sample timing and off-center turns");
}

static point_t inset(point_t from, point_t toward, int pixels)
{
    const int x = toward.x - from.x, y = toward.y - from.y;
    const int length = abs(x) > abs(y) ? abs(x) : abs(y);
    if (!length) return from;
    if (pixels > length / 3) pixels = length / 3;
    return (point_t){from.x + x * pixels / length, from.y + y * pixels / length};
}

static void test_sparse_rounded_trace(void)
{
    static const char *const words[] = {"changjiang", "xiongmao", "mudidi", "guangzhou"};
    unsigned total = 0, top_three = 0;
    for (size_t i = 0; i < sizeof(words) / sizeof(words[0]); ++i) {
        const size_t length = strlen(words[i]);
        for (int trial = 0; trial < 8; ++trial) {
            point_t keys[APP_PINYIN_SWIPE_MAX_SPELLING];
            for (size_t key = 0; key < length; ++key) {
                const point_t center = centers[words[i][key] - 'a'];
                keys[key] = (point_t){center.x - 9 + trial * 2, center.y + 11 + trial};
            }
            /* One sample per letter, shifted as a thumb may land and cutting
             * inside the corners, with contact starting and ending inboard. */
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS] = {inset(keys[0], keys[1], 12)};
            for (size_t key = 1; key + 1 < length; ++key) {
                const point_t a = inset(keys[key], keys[key - 1], 17);
                const point_t b = inset(keys[key], keys[key + 1], 17);
                points[key] = (point_t){(a.x + b.x + 2 * keys[key].x) / 4,
                                       (a.y + b.y + 2 * keys[key].y) / 4};
            }
            points[length - 1] = inset(keys[length - 1], keys[length - 2], 14);
            char matches[3][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            const size_t found = app_pinyin_swipe_decode(points, length, centers, matches, 3);
            ++total;
            for (size_t j = 0; j < found; ++j) {
                if (strcmp(matches[j], words[i])) continue;
                ++top_three;
                break;
            }
        }
    }
    assert(top_three >= 30 && total == 32);

    const point_t adjacent[] = {inset(centers['e' - 'a'], centers['r' - 'a'], 9),
                               inset(centers['r' - 'a'], centers['e' - 'a'], 9)};
    char matches[3][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
    assert(app_pinyin_swipe_decode(adjacent, 2, centers, matches, 3) && !strcmp(matches[0], "er"));
    puts("PASS short adjacent-key stroke, clipped endpoints, sparse samples and consistently shifted rounded turns");
}

static void test_sentence_swipes(void)
{
    static const struct { const char *spelling; const char *text; } sentences[] = {
        {"woxianghechayexiangtingyinyue", "我想喝茶也想听音乐"},
        {"jintianwomenyiqiqugongyuan", "今天我们一起去公园"},
        {"woxiangdakaishoujichakantianqiyubao", "我想打开手机查看天气预报"},
    };
    unsigned rounded_matches = 0;
    for (size_t entry = 0; entry < sizeof(sentences) / sizeof(sentences[0]); ++entry) {
        const char *spelling = sentences[entry].spelling;
        const size_t length = strlen(spelling);
        for (unsigned step = 0; step < 3; ++step) {
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
            size_t count = 0;
            for (size_t i = 0; i < length; ++i) {
                point_t key = centers[spelling[i] - 'a'];
                if (step == 1 && i) {
                    const point_t previous = centers[spelling[i - 1] - 'a'];
                    for (int sample = 1; sample < 8; ++sample) {
                        add_sample(points, &count, (point_t){
                            previous.x + (key.x - previous.x) * sample / 8,
                            previous.y + (key.y - previous.y) * sample / 8});
                    }
                } else if (step == 2 && i && i + 1 < length) {
                    const point_t a = inset(key, centers[spelling[i - 1] - 'a'], 12);
                    const point_t b = inset(key, centers[spelling[i + 1] - 'a'], 12);
                    key = (point_t){(a.x + b.x + 2 * key.x) / 4 + 6,
                                    (a.y + b.y + 2 * key.y) / 4 + 8};
                }
                add_sample(points, &count, key);
            }
            char matches[4][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            const size_t found = app_pinyin_swipe_decode(points, count, centers, matches, 4);
            bool correct = false;
            for (size_t i = 0; i < found; ++i) {
                for (size_t j = 0; j < i; ++j) assert(strcmp(matches[i], matches[j]));
                if (!strcmp(matches[i], spelling)) correct = true;
            }
            if (step == 0) assert(correct && !strcmp(matches[0], spelling));
            else if (step == 1) assert(correct);
            else if (correct) ++rounded_matches;
        }
        app_pinyin_candidate_t candidate;
        assert(app_pinyin_lookup(spelling, 0, &candidate, 1, NULL) == 1);
        assert(candidate.consumed == length && !strcmp(candidate.text, sentences[entry].text));
    }
    assert(rounded_matches >= 2);
    printf("PASS three long sentences with sparse and compacted strokes, %u/3 rounded, unique alternates and Chinese text\n",
           rounded_matches);
}

static size_t draw_rounded_sentence(point_t points[APP_PINYIN_SWIPE_MAX_POINTS], const char *spelling,
                                   unsigned variation)
{
    const size_t length = strlen(spelling);
    size_t count = 0;
    for (size_t i = 0; i < length; ++i) {
        point_t key = centers[spelling[i] - 'a'];
        if (variation && i && i + 1 < length) {
            const int radius = 4 + 3 * (int)variation;
            const point_t a = inset(key, centers[spelling[i - 1] - 'a'], radius);
            const point_t b = inset(key, centers[spelling[i + 1] - 'a'], radius);
            key = (point_t){(a.x + b.x + 2 * key.x) / 4 + 5 - 3 * (int)variation,
                            (a.y + b.y + 2 * key.y) / 4 + 5 + 2 * (int)variation};
        }
        add_sample(points, &count, key);
    }
    return count;
}

static size_t draw_without_pauses(point_t points[APP_PINYIN_SWIPE_MAX_POINTS], const char *spelling)
{
    point_t corners[APP_PINYIN_SWIPE_MAX_POINTS];
    const size_t count = draw_rounded_sentence(corners, spelling, 1);
    size_t written = 0;
    add_sample(points, &written, corners[0]);
    for (size_t i = 1; i < count; ++i) {
        const point_t a = corners[i - 1], b = corners[i];
        const int steps = 1 + (abs(a.x - b.x) + abs(a.y - b.y)) / 22;
        for (int step = 1; step <= steps; ++step)
            add_sample(points, &written, (point_t){a.x + (b.x - a.x) * step / steps,
                                                   a.y + (b.y - a.y) * step / steps});
    }
    return written;
}

static void test_medium_sentence_swipes(void)
{
    static const struct { const char *spelling; const char *text; } sentences[] = {
        {"nichifanlema", "你吃饭了吗"},
        {"nihewoqukanshu", "你和我去看书"},
    };
    for (size_t i = 0; i < sizeof(sentences) / sizeof(sentences[0]); ++i) {
        for (unsigned variation = 0; variation < 5; ++variation) {
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
            const size_t count = variation == 4 ? draw_without_pauses(points, sentences[i].spelling) :
                draw_rounded_sentence(points, sentences[i].spelling, variation);
            char matches[APP_PINYIN_SWIPE_MAX_MATCHES][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            const size_t found = app_pinyin_swipe_decode(points, count, centers, matches, APP_PINYIN_SWIPE_MAX_MATCHES);
            bool recognized = false;
            for (size_t j = 0; j < found; ++j)
                if (!strcmp(matches[j], sentences[i].spelling)) recognized = true;
            assert(recognized);
            if (variation == 0 || variation == 4) assert(!strcmp(matches[0], sentences[i].spelling));
        }
        app_pinyin_candidate_t candidate;
        assert(app_pinyin_lookup(sentences[i].spelling, 0, &candidate, 1, NULL) == 1);
        assert(candidate.consumed == strlen(sentences[i].spelling) && !strcmp(candidate.text, sentences[i].text));
    }

    /* These alternatives share an initial sweep and the same endpoints. The
     * route from C through H, I and F before A distinguishes the longer one. */
    static const char *alternatives[] = {"nicianlema", "nichifanlema"};
    for (size_t i = 0; i < sizeof(alternatives) / sizeof(alternatives[0]); ++i) {
        point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
        const size_t count = draw_without_pauses(points, alternatives[i]);
        const char *matches[2];
        assert(app_pinyin_swipe_match(points, count, centers, 2, word_at, alternatives, matches, 2));
        assert(!strcmp(matches[0], alternatives[i]));
    }
    puts("PASS medium sentences with sparse, rounded and near-constant-speed traces, route discrimination and Chinese text");
}

static void test_biased_sentence_swipes(void)
{
    static const struct {
        const char *spelling;
        unsigned expected;
    } cases[] = {
        {"nichifanlema", 4},
        {"nihewoqukanshu", 6},
        {"jintianwomenyiqiqugongyuan", 6},
    };
    /* Start and release away from key centers. Alternate between densely
     * sampled motion and corner-cutting with only a few reported points. */
    static const point_t shifts[] = {{12, 8}, {-12, -8}, {18, 9}, {-18, 9}, {-7, 22}, {7, -22}};
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        unsigned recognized = 0;
        for (size_t variant = 0; variant < sizeof(shifts) / sizeof(shifts[0]); ++variant) {
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
            const size_t count = variant % 2 ? draw_rounded_sentence(points, cases[i].spelling, 2) :
                draw_without_pauses(points, cases[i].spelling);
            for (size_t point = 0; point < count; ++point) {
                points[point].x += shifts[variant].x;
                points[point].y += shifts[variant].y;
            }
            char matches[4][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            const size_t found = app_pinyin_swipe_decode(points, count, centers, matches, 4);
            for (size_t j = 0; j < found; ++j)
                if (!strcmp(matches[j], cases[i].spelling)) {
                    ++recognized;
                    break;
                }
        }
        assert(recognized >= cases[i].expected);
    }
    puts("PASS spatially biased long gestures, variable sampling and off-center endpoints");
}

static void test_shallow_h_routes(void)
{
    /* Centers for the 472px-wide three-row keyboard, in alphabetic order. */
    static const point_t keyboard[] = {
        {50, 359}, {287, 405}, {191, 405}, {144, 359}, {120, 313}, {191, 359}, {239, 359},
        {286, 359}, {358, 313}, {333, 359}, {380, 359}, {427, 359}, {382, 405}, {335, 405},
        {405, 313}, {453, 313}, {25, 313}, {168, 313}, {97, 359}, {215, 313}, {310, 313},
        {239, 405}, {73, 313}, {143, 405}, {263, 313}, {96, 405},
    };
    static const struct { const char *spelling, *text; } cases[] = {
        {"nichilema", "你吃了吗"},
        {"nichiwufanlema", "你吃午饭了吗"},
        {"diyici", "第一次"},
        {"zhongwencidian", "中文词典"},
    };
    static const struct { int x, y, steps, i_offset; } variations[] = {
        {0, 0, 1, 0}, {0, 0, 3, 0}, {0, 0, 8, 0}, {12, -5, 3, 0},
        {12, -5, 3, -6}, {-12, -5, 3, 0}, {14, 8, 5, 0}, {6, 10, 2, 0},
    };
    for (size_t i = 0; i < sizeof(cases) / sizeof(cases[0]); ++i) {
        const char *spelling = cases[i].spelling;
        for (size_t j = 0; j < sizeof(variations) / sizeof(variations[0]); ++j) {
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
            size_t count = 0;
            const int x = variations[j].x, y = variations[j].y, steps = variations[j].steps;
            point_t previous = keyboard[spelling[0] - 'a'];
            previous.x += x;
            previous.y += y;
            add_sample(points, &count, previous);
            for (size_t key = 1; spelling[key]; ++key) {
                point_t next = keyboard[spelling[key] - 'a'];
                next.x += x + (spelling[key] == 'i' ? variations[j].i_offset : 0);
                next.y += y;
                for (int step = 1; step <= steps; ++step)
                    add_sample(points, &count, (point_t){
                        previous.x + (next.x - previous.x) * step / steps,
                        previous.y + (next.y - previous.y) * step / steps,
                    });
                previous = next;
            }
            char matches[4][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            assert(app_pinyin_swipe_decode(points, count, keyboard, matches, 4) && !strcmp(matches[0], spelling));
            app_pinyin_candidate_t candidate;
            assert(app_pinyin_lookup(matches[0], 0, &candidate, 1, NULL) == 1);
            assert(candidate.consumed == strlen(spelling) && !strcmp(candidate.text, cases[i].text));
        }
    }
    puts("PASS shallow C-H-I meal routes and genuine CI words on the enlarged keyboard");
}

static void test_compaction_keeps_early_bends(void)
{
    point_t points[APP_PINYIN_SWIPE_MAX_POINTS] = {{30, 130}, {60, 140}, {90, 140}, {120, 150}};
    for (size_t i = 4; i < APP_PINYIN_SWIPE_MAX_POINTS; ++i)
        points[i] = (point_t){points[i - 1].x + 4, 150};
    const point_t last = points[APP_PINYIN_SWIPE_MAX_POINTS - 1];
    const size_t count = app_pinyin_swipe_compact(points, APP_PINYIN_SWIPE_MAX_POINTS);
    assert(count <= (2 * APP_PINYIN_SWIPE_MAX_POINTS + 2) / 3);
    assert(points[0].x == 30 && points[0].y == 130);
    assert(points[1].x == 60 && points[1].y == 140);
    assert(points[2].x == 90 && points[2].y == 140);
    assert(points[count - 1].x == last.x && points[count - 1].y == last.y);
    puts("PASS compaction preserves shallow early bends and outer endpoints");
}

static void test_question_pronunciations(void)
{
    static const char *const words[] = {"nishishui", "nishishei"};
    for (size_t word = 0; word < sizeof(words) / sizeof(words[0]); ++word) {
        for (int offset = 0; offset <= 7; offset += 7) {
            point_t points[APP_PINYIN_SWIPE_MAX_POINTS];
            size_t count = 0;
            for (size_t key = 0; words[word][key]; ++key) {
                const point_t center = centers[words[word][key] - 'a'];
                points[count++] = (point_t){center.x + offset, center.y - offset};
            }
            char matches[3][APP_PINYIN_SWIPE_MAX_SPELLING + 1];
            const size_t found = app_pinyin_swipe_decode(points, count, centers, matches, 3);
            assert(found && !strcmp(matches[0], words[word]));
            app_pinyin_candidate_t candidate;
            assert(app_pinyin_lookup(matches[0], 0, &candidate, 1, NULL) == 1);
            assert(candidate.consumed == strlen(words[word]) && !strcmp(candidate.text, "你是谁"));
        }
    }
    puts("PASS both pronunciations of 你是谁 with sparse centered and offset traces");
}

static void check_dense_swipe(const char *word, int steps, int noise)
{
    point_t points[APP_PINYIN_SWIPE_MAX_POINTS] = {centers[word[0] - 'a']};
    size_t count = 1;
    unsigned compactions = 0;
    for (size_t i = 1; word[i]; ++i) {
        const point_t start = centers[word[i - 1] - 'a'], end = centers[word[i] - 'a'];
        for (int step = 1; step <= steps; ++step) {
            const int jitter = step == steps ? 0 : (step % 3 - 1) * noise;
            const point_t point = {
                start.x + (end.x - start.x) * step / steps + jitter,
                start.y + (end.y - start.y) * step / steps - jitter,
            };
            const point_t last = points[count - 1];
            const int dx = point.x - last.x, dy = point.y - last.y;
            if (dx * dx + dy * dy < 16) continue;
            if (count == APP_PINYIN_SWIPE_MAX_POINTS) {
                count = app_pinyin_swipe_compact(points, count);
                assert(count >= 2 && count < APP_PINYIN_SWIPE_MAX_POINTS);
                assert(points[count - 1].x == last.x && points[count - 1].y == last.y);
                ++compactions;
            }
            points[count++] = point;
        }
    }
    const char *match;
    assert(compactions && app_pinyin_swipe_match(points, count, centers, app_pinyin_spelling_count(),
        pinyin_word_at, NULL, &match, 1) == 1 && !strcmp(match, word));
}

int main(void)
{
    layout();
    test_phrases();
    test_disambiguation();
    test_limits();
    test_dictionary();
    test_extended_swipes();
    test_sparse_rounded_trace();
    test_sentence_swipes();
    test_medium_sentence_swipes();
    test_biased_sentence_swipes();
    test_shallow_h_routes();
    test_question_pronunciations();
    test_compaction_keeps_early_bends();
    check_dense_swipe("shezhinaozhong", 20, 3);
    check_dense_swipe("wuxianwangluo", 24, 1);
    puts("PASS over-capacity swipes retain turns and recognize long dictionary phrases");
    return 0;
}
