#include "app_text.h"
#include <assert.h>
#include <locale.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <wchar.h>

static void expect(const char *input, size_t length, const char *expected)
{
    char buffer[64];
    assert(length < sizeof(buffer) - 1);
    memset(buffer, 0x5A, sizeof(buffer));
    memcpy(buffer, input, length);
    const size_t output = app_text_sanitize_utf8(buffer, length);
    assert(output <= length && output == strlen(expected));
    assert(strcmp(buffer, expected) == 0 && buffer[length + 1] == 0x5A);
}

int main(void)
{
    assert(setlocale(LC_CTYPE, "C.UTF-8"));
    expect("", 0, "");
    expect("plain", 5, "plain");
    expect("\xE4\xB8\xAD\xE6\x96\x87", 6, "\xE4\xB8\xAD\xE6\x96\x87");
    expect("\xF0\x9F\x98\x80", 4, "\xF0\x9F\x98\x80");
    expect("\xEF\xBB\xBFtext", 7, "text");
    expect("a\xE4\xB8", 3, "a");
    expect("a\xF0\x9F\x98", 4, "a");
    expect("\xED\xA0\x80", 3, "???");
    expect("\xC0\x80", 2, "??");
    expect("\xF4\x90\x80\x80", 4, "????");
    expect("a\0b", 3, "a");
    assert(app_text_sanitize_utf8(NULL, 0) == 0);
    puts("PASS UTF-8 preservation / BOM / partial / invalid sequences");

    char full[8194];
    memset(full, 'a', sizeof(full));
    memcpy(full + 8189, "\xE4\xB8\xAD", 3);
    assert(app_text_sanitize_utf8(full, 8192) == 8192);
    assert(memcmp(full + 8189, "\xE4\xB8\xAD", 3) == 0 && full[8193] == 'a');
    puts("PASS complete code point at 8KiB boundary retained");

    uint32_t random = 7;
    for (unsigned run = 0; run < 20000; ++run) {
        char buffer[130], copy[130];
        const size_t length = run % 128;
        memset(buffer, 0x5A, sizeof(buffer));
        for (size_t i = 0; i < length; ++i) {
            random = random * 1664525U + 1013904223U;
            buffer[i] = (char)(random >> 24);
        }
        size_t output = app_text_sanitize_utf8(buffer, length);
        assert(output <= length && buffer[output] == '\0' && buffer[length + 1] == 0x5A);
        const char *source = buffer;
        mbstate_t state = {0};
        assert(mbsrtowcs(NULL, &source, 0, &state) != (size_t)-1);
        memcpy(copy, buffer, output + 1);
        assert(app_text_sanitize_utf8(buffer, output) == output && strcmp(copy, buffer) == 0);
    }
    puts("PASS 20000 malformed input cases: libc UTF-8 validation / bounds / idempotence");
    return 0;
}
