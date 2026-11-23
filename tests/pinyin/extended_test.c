#include "app_pinyin.h"
#include "app_pinyin_extended.h"

#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { HEADER = 40, KEYS = 64, WORDS = 73, SIZE = 83 };

static void write_u32(uint8_t *bytes, uint32_t value)
{
    for (unsigned i = 0; i < 4; ++i) bytes[i] = (uint8_t)(value >> (8 * i));
}

static void update_crc(uint8_t data[SIZE])
{
    uint32_t crc = UINT32_MAX;
    for (size_t i = HEADER; i < SIZE; ++i) {
        crc ^= data[i];
        for (unsigned bit = 0; bit < 8; ++bit)
            crc = (crc >> 1) ^ (crc & 1U ? UINT32_C(0xedb88320) : 0);
    }
    write_u32(data + 12, ~crc);
}

static void fixture(uint8_t data[SIZE])
{
    memset(data, 0, SIZE);
    memcpy(data, "PYN2", 4);
    write_u32(data + 4, 1);
    write_u32(data + 8, SIZE);
    write_u32(data + 16, 2);
    write_u32(data + 20, 2);
    write_u32(data + 24, HEADER);
    write_u32(data + 28, KEYS);
    write_u32(data + 32, WORDS);
    write_u32(data + 48, 4);
    write_u32(data + 52, 5);
    write_u32(data + 56, 9);
    write_u32(data + 60, 10);
    memcpy(data + KEYS, "abc\0defg", 9);
    const uint8_t records[] = {24, 0x29, 0x59, 0x14, 0x6c, 7, 0x16, 0x4e, 0x4c, 0x75};
    memcpy(data + WORDS, records, sizeof(records));
    update_crc(data);
}

static void check_first(const char *spelling, const char *text)
{
    app_pinyin_candidate_t candidate;
    assert(app_pinyin_lookup(spelling, 0, &candidate, 1, NULL) == 1);
    assert(candidate.consumed == strlen(spelling));
    assert(!strcmp(candidate.text, text));
}

static void check_corruption(void)
{
    uint8_t data[SIZE];
    assert(app_pinyin_extended_count() == 0);
    check_first("nihao", "你好");
    fixture(data);
    assert(app_pinyin_extended_test_load(data, sizeof(data)));
    assert(app_pinyin_extended_count() == 2 && app_pinyin_extended_priority(0) == 6);
    assert(!strcmp(app_pinyin_extended_key(0), "abc") && !strcmp(app_pinyin_extended_key(1), "defg"));
    assert(app_pinyin_extended_lower_bound("def", 3, 0) == 1);
    assert(app_pinyin_extended_lower_bound("zz", 1, 0) == 2);
    check_first("abc", "天气");
    check_first("defg", "世界");
    check_first("abcnihao", "天气你好");

    data[WORDS] ^= 1;
    assert(!app_pinyin_extended_test_load(data, sizeof(data)) && app_pinyin_extended_count() == 0);
    fixture(data);
    assert(!app_pinyin_extended_test_load(data, sizeof(data) - 1));
    write_u32(data + 48, UINT32_MAX);
    update_crc(data);
    assert(!app_pinyin_extended_test_load(data, sizeof(data)));
    fixture(data);
    data[KEYS + 4] = 'a';
    data[KEYS + 5] = 'a';
    update_crc(data);
    assert(!app_pinyin_extended_test_load(data, sizeof(data)));
    fixture(data);
    data[WORDS] |= 0x80;
    update_crc(data);
    assert(!app_pinyin_extended_test_load(data, sizeof(data)));
    assert(!app_pinyin_extended_key(0));
    check_first("nihao", "你好");
}

static void check_image(const char *path)
{
    FILE *file = fopen(path, "rb");
    assert(file && fseek(file, 0, SEEK_END) == 0);
    const long size = ftell(file);
    assert(size > 0 && size <= 4 * 1024 * 1024 && fseek(file, 0, SEEK_SET) == 0);
    uint8_t *bytes = malloc((size_t)size);
    assert(bytes && fread(bytes, 1, (size_t)size, file) == (size_t)size);
    assert(fclose(file) == 0);
    assert(app_pinyin_extended_test_load(bytes, (size_t)size));
    const size_t count = app_pinyin_extended_count();
    assert(count > 80000);
    check_first("shoujike", "手机壳");
    check_first("chongdianbao", "充电宝");
    check_first("rengongzhineng", "人工智能");
    check_first("nihao", "你好");
    check_first("nichilema", "你吃了吗");
    check_first("nichiwufanlema", "你吃午饭了吗");
    app_pinyin_candidate_t candidates[8];
    const size_t meals = app_pinyin_lookup("nichilema", 0, candidates, 8, NULL);
    for (size_t i = 1; i < meals; ++i) assert(strcmp(candidates[i].text, "你吃了吗"));
    assert(!app_pinyin_extended_test_load(NULL, 0));
    free(bytes);
    printf("Extended dictionary validated all %zu spelling records and production lookup.\n", count);
}

int main(int argc, char **argv)
{
    check_corruption();
    if (argc == 2) check_image(argv[1]);
    puts("Pinyin extension format, bounds, fallback and sentence integration passed.");
    return 0;
}
