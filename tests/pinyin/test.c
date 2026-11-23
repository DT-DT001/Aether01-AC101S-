#include "app_pinyin.h"

#include <assert.h>
#include <stdio.h>
#include <string.h>

static void first(const char *spelling, const char *expected, size_t consumed)
{
    app_pinyin_candidate_t candidate = {0};
    assert(app_pinyin_lookup(spelling, 0, &candidate, 1, NULL) == 1);
    assert(strcmp(candidate.text, expected) == 0);
    assert(candidate.consumed == consumed);
}

static void check_pages(const char *spelling)
{
    app_pinyin_candidate_t all[256] = {0};
    bool more = true;
    const size_t capacity = sizeof(all) / sizeof(all[0]);
    const size_t count = app_pinyin_lookup(spelling, 0, all, capacity, &more);
    assert(count > 5 && !more);
    size_t offset = 0;
    do {
        app_pinyin_candidate_t page[5] = {0};
        const size_t read = app_pinyin_lookup(spelling, offset, page, 5, &more);
        assert(read && read <= 5 && read <= count - offset);
        for (size_t i = 0; i < read; ++i) {
            assert(strcmp(all[offset + i].text, page[i].text) == 0);
            assert(all[offset + i].consumed == page[i].consumed);
        }
        offset += read;
    } while (more);
    assert(offset == count);
    assert(app_pinyin_lookup(spelling, count, all, capacity, &more) == 0 && !more);
    assert(app_pinyin_lookup(spelling, SIZE_MAX, all, capacity, &more) == 0 && !more);
}

static void check_sentence(const char *spelling, const char *expected,
                           const char *first_word, size_t first_letters)
{
    const size_t letters = strlen(spelling);
    first(spelling, expected, letters);
    app_pinyin_candidate_t partial;
    bool more = false;
    assert(app_pinyin_lookup(spelling, 1, &partial, 1, &more) == 1 && more);
    assert(partial.consumed == first_letters && !strcmp(partial.text, first_word));
    first(spelling + partial.consumed, expected + strlen(first_word), letters - partial.consumed);
}

static void check_sentence_limits(void)
{
    check_sentence("woxianghechayexiangtingyinyue", "我想喝茶也想听音乐", "我想", 7);
    check_sentence("jintianwomenyiqiqugongyuan", "今天我们一起去公园", "今天", 7);
    check_sentence("mingtianwoxianggenniyiqiqugongyuan", "明天我想跟你一起去公园", "明天", 8);
    check_sentence("woxiangdakaishoujichakantianqiyubao", "我想打开手机查看天气预报", "我想", 7);
    check_sentence("jintianwoxiangdakaishoujichakantianqiyubao", "今天我想打开手机查看天气预报", "今天", 7);
    check_sentence("shijianshiyigehenzhongyaodewenti", "时间是一个很重要的问题", "时间", 7);
    first("woxianghechayexiangtingyinyuexqz", "我想喝茶也想听音乐", 29);

    const char full[] = "woxiangdakaishoujichakantianqiyubao"
                        "woxiangdakaishoujichakantianqiyubaoqu";
    assert(strlen(full) == APP_PINYIN_MAX_SPELLING);
    const char *prefix = "我想打开手机查看天气预报我想打开手机查看";
    first(full, prefix, 59);
    first(full + 59, "天气预报去", 13);
    check_pages("mingtianwoxianggenniyiqiqugongyuan");
    puts("PASS sentence conversion, exact partial consumption, pagination and long-buffer boundaries");
}

static void check_catalog(void)
{
    const size_t count = app_pinyin_spelling_count();
    assert(app_pinyin_syllable_count() >= 410 && count >= 890);
    for (size_t i = 0; i < count; ++i) {
        const char *spelling = app_pinyin_spelling_at(i);
        const size_t length = strlen(spelling);
        assert(length && length <= APP_PINYIN_MAX_SPELLING);
        for (size_t j = 0; j < length; ++j) assert(spelling[j] >= 'a' && spelling[j] <= 'z');
        for (size_t j = i + 1; j < count; ++j) assert(strcmp(app_pinyin_spelling_at(j), spelling));
        app_pinyin_candidate_t candidate;
        assert(app_pinyin_lookup(spelling, 0, &candidate, 1, NULL) == 1);
        assert(candidate.text[0] && candidate.consumed == length);
    }
    assert(app_pinyin_spelling_at(count) == NULL);
    assert(app_pinyin_spelling_at(SIZE_MAX) == NULL);

    const size_t common_count = app_pinyin_common_phrase_count();
    assert(common_count >= 14000);
    const char *previous = "";
    for (size_t i = 0; i < common_count; ++i) {
        const char *spelling = app_pinyin_common_phrase_spelling_at(i);
        assert(spelling && strcmp(previous, spelling) < 0);
        assert(app_pinyin_common_phrase_priority(i) <= 15);
        previous = spelling;
        if (i % 256 <= 1 || i % 256 == 255 || i + 1 == common_count) {
            app_pinyin_candidate_t candidate;
            assert(app_pinyin_lookup(spelling, 0, &candidate, 1, NULL) == 1);
            assert(candidate.text[0] && candidate.consumed == strlen(spelling));
        }
    }
    assert(app_pinyin_common_phrase_spelling_at(common_count) == NULL);
    assert(app_pinyin_common_phrase_priority(SIZE_MAX) == 0);
}

int main(void)
{
    first("ni", "你", 2);
    first("zhong", "中", 5);
    first("shuang", "双", 6);
    first("nv", "女", 2);
    first("hei", "黑", 3);
    first("tie", "铁", 3);
    first("za", "杂", 2);
    first("zang", "藏", 4);
    first("ei", "欸", 2);
    first("den", "扽", 3);
    first("dia", "嗲", 3);
    first("nihao", "你好", 5);
    first("nishi", "你是", 5);
    first("nishishui", "你是谁", 9);
    first("nishishei", "你是谁", 9);
    first("nichilema", "你吃了吗", 9);
    first("nichifanlema", "你吃饭了吗", 12);
    first("nichiwufanlema", "你吃午饭了吗", 14);
    first("chiwufan", "吃午饭", 8);
    first("ci", "此", 2);
    first("diyici", "第一次", 6);
    first("zhongguo", "中国", 8);
    first("xiexie", "谢谢", 6);
    first("wuxianwangluo", "无线网络", 13);
    first("tianqiyubao", "天气预报", 11);
    first("shezhinaozhong", "设置闹钟", 14);
    first("mudidi", "目的地", 6);
    first("yifenzhong", "一分钟", 10);
    first("shuoshihua", "说实话", 10);
    puts("PASS single syllables and common phrases are offered first");

    first("niha", "你好", 4);
    first("nihaoq", "你好", 5);
    first("woainizhongguo", "我爱你中国", 14);
    first("zhongguorenninhao", "中国人您好", 17);
    app_pinyin_candidate_t candidates[8] = {0};
    size_t offers = app_pinyin_lookup("chi", 0, candidates, 8, NULL);
    bool dinner = false;
    for (size_t i = 0; i < offers; ++i)
        if (!strcmp(candidates[i].text, "吃饭")) dinner = candidates[i].consumed == 3;
    assert(dinner);
    offers = app_pinyin_lookup("nichi", 0, candidates, 8, NULL);
    bool lunch = false;
    for (size_t i = 0; i < offers; ++i)
        if (!strcmp(candidates[i].text, "你吃午饭了吗")) lunch = candidates[i].consumed == 5;
    assert(lunch);
    const size_t count = app_pinyin_lookup("nihao", 0, candidates, 8, NULL);
    assert(count >= 2 && !strcmp(candidates[1].text, "你") && candidates[1].consumed == 2);
    const size_t xian_count = app_pinyin_lookup("xian", 0, candidates, 8, NULL);
    assert(xian_count == 8 && !strcmp(candidates[3].text, "西安") && candidates[3].consumed == 4);
    first("hao", "好", 3); /* Remainder after selecting 你 from nihao. */
    assert(app_pinyin_lookup("xyzzy", 0, candidates, 8, NULL) == 0);
    assert(app_pinyin_lookup("NIHAO", 0, candidates, 8, NULL) == 0);
    assert(app_pinyin_lookup("ni+hao", 0, candidates, 8, NULL) == 0);
    assert(app_pinyin_lookup("", 0, candidates, 8, NULL) == 0);
    assert(app_pinyin_lookup(NULL, 0, candidates, 8, NULL) == 0);
    puts("PASS complete-segment composition, partial choices, prefix suggestions and unknown input");

    check_pages("shi");
    check_pages("xian");
    check_pages("z");
    char too_long[APP_PINYIN_MAX_SPELLING + 2] = {0};
    memset(too_long, 'a', sizeof(too_long) - 1);
    assert(app_pinyin_lookup(too_long, 0, candidates, 8, NULL) == 0);
    too_long[APP_PINYIN_MAX_SPELLING] = 0;
    assert(app_pinyin_lookup(too_long, 0, candidates, 8, NULL) >= 1);
    assert(strlen(candidates[0].text) <= APP_PINYIN_CANDIDATE_BYTES - 1);
    assert(candidates[0].consumed && candidates[0].consumed < APP_PINYIN_MAX_SPELLING);
    check_sentence_limits();
    check_catalog();
    printf("PASS paginated output, maximum-sized composition, %zu core and %zu packed spellings\n",
           app_pinyin_spelling_count(), app_pinyin_common_phrase_count());
    return 0;
}
