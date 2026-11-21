#include "app_text.h"
#include <stdint.h>
#include <stdbool.h>

size_t app_text_sanitize_utf8(char *text, size_t length)
{
    if (!text) return 0;
    size_t in = 0, out = 0;
    if (length >= 3 && (uint8_t)text[0] == 0xEF &&
        (uint8_t)text[1] == 0xBB && (uint8_t)text[2] == 0xBF) in = 3;
    while (in < length) {
        const uint8_t lead = (uint8_t)text[in];
        if (!lead) break;
        if (lead < 0x80) {
            text[out++] = text[in++];
            continue;
        }
        const size_t count = lead >= 0xC2 && lead <= 0xDF ? 2 :
                             lead >= 0xE0 && lead <= 0xEF ? 3 :
                             lead >= 0xF0 && lead <= 0xF4 ? 4 : 0;
        bool valid = count != 0;
        const size_t available = length - in;
        for (size_t i = 1; valid && i < count && i < available; ++i) {
            const uint8_t byte = (uint8_t)text[in + i];
            valid = byte >= 0x80 && byte <= 0xBF;
            if (i == 1 && ((lead == 0xE0 && byte < 0xA0) ||
                          (lead == 0xED && byte >= 0xA0) ||
                          (lead == 0xF0 && byte < 0x90) ||
                          (lead == 0xF4 && byte >= 0x90))) valid = false;
        }
        if (valid && available < count) break;
        if (!valid) {
            text[out++] = '?';
            ++in;
            continue;
        }
        for (size_t i = 0; i < count; ++i) text[out++] = text[in++];
    }
    text[out] = '\0';
    return out;
}
