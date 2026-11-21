#include "app_reader_layout.h"

#include <assert.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

static uint16_t width(uint32_t codepoint, uint32_t next, void *context)
{
    (void)next; (void)context;
    return codepoint < 0x80U ? 5 : codepoint > 0xFFFFU ? 12 : 10;
}

static void pages_preserve_text(void)
{
    const char *input = "中文阅读小说甲乙丙丁戊己庚辛壬癸\xF0\xA0\x80\x80" "English123中文继续";
    char reconstructed[1024] = {0};
    size_t offset = 0, restored = 0;
    while (offset < strlen(input)) {
        char page[128];
        const app_reader_page_t result = app_reader_layout_page(input + offset,
            strlen(input) - offset, page, sizeof(page), 25, 26, 12, 2, width, NULL);
        assert(result.consumed > 0 && result.consumed <= strlen(input) - offset);
        assert(result.lines <= 2 && result.written == strlen(page));
        for (size_t i = 0; page[i]; ++i) if (page[i] != '\n') reconstructed[restored++] = page[i];
        offset += result.consumed;
        assert(!input[offset] || ((uint8_t)input[offset] & 0xC0U) != 0x80U);
    }
    assert(strcmp(input, reconstructed) == 0);
}

int main(void)
{
    char output[128];
    const char *chinese = "甲乙丙丁戊己";
    app_reader_page_t page = app_reader_layout_page(chinese, strlen(chinese), output,
        sizeof(output), 20, 24, 12, 0, width, NULL);
    assert(page.consumed == 12 && page.lines == 2 && strcmp(output, "甲乙\n丙丁") == 0);
    page = app_reader_layout_page(chinese + page.consumed, 6, output, sizeof(output),
                                  20, 24, 12, 0, width, NULL);
    assert(page.consumed == 6 && strcmp(output, "戊己") == 0);
    puts("PASS measured CJK widths, page height, exact source offset");
    page = app_reader_layout_page("甲乙，丙", strlen("甲乙，丙"), output, sizeof(output),
                                  20, 36, 12, 0, width, NULL);
    assert(strcmp(output, "甲\n乙，\n丙") == 0);
    puts("PASS Chinese closing punctuation stays with the preceding glyph");

    const char *mixed = "A\r\nB\rC\nD";
    page = app_reader_layout_page(mixed, strlen(mixed), output, sizeof(output),
                                  30, 24, 12, 0, width, NULL);
    assert(page.consumed == 5 && strcmp(output, "A\nB") == 0);
    page = app_reader_layout_page("\r\n\r\nZ", 5, output, sizeof(output),
                                  30, 24, 12, 0, width, NULL);
    assert(page.consumed == 4 && strcmp(output, "\n") == 0);
    puts("PASS CRLF/CR/LF and blank lines consume original bytes");

    page = app_reader_layout_page("\xEF\xBB\xBF" "A\tB", 6, output,
                                  sizeof(output), 30, 24, 12, 0, width, NULL);
    assert(page.consumed == 6 && strcmp(output, "A    B") == 0);
    assert(app_reader_layout_page("甲", 3, output, 3, 20, 24, 12, 0, width, NULL).consumed == 0);
    assert(app_reader_layout_page("甲", 3, output, 4, 20, 24, 12, 0, width, NULL).consumed == 3);
    assert(app_reader_layout_page("A", 1, output, sizeof(output), 20, 11, 12, 0, width, NULL).consumed == 0);
    assert(app_reader_layout_page("A", 1, output, sizeof(output), 20, 20, 12, UINT32_MAX, width, NULL).consumed == 0);
    puts("PASS BOM/tabs/capacity/invalid geometry");

    pages_preserve_text();
    puts("PASS multi-page UTF-8 reconstruction including supplementary Han");
    uint32_t random = 123;
    for (unsigned test = 0; test < 20000; ++test) {
        char input[128], guarded[130];
        const size_t length = test % sizeof(input);
        for (size_t i = 0; i < length; ++i) {
            random = random * 1664525U + 1013904223U;
            input[i] = (char)(random >> 24);
        }
        memset(guarded, 0x5A, sizeof(guarded));
        const size_t capacity = 2U + test % 126U;
        page = app_reader_layout_page(input, length, guarded + 1, capacity,
                                       5 + test % 60, 12 + test % 100, 12, test % 8, width, NULL);
        assert(page.consumed <= length && page.written < capacity);
        assert(guarded[0] == 0x5A && guarded[capacity + 1U] == 0x5A);
        assert(guarded[page.written + 1U] == '\0');
    }
    puts("PASS 20000 malformed-text/capacity cases with ASan/UBSan");
    return 0;
}
