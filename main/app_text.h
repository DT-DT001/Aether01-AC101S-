#pragma once
#include <stddef.h>

/* Caller provides length+1 writable bytes. Preserve complete UTF-8 and discard
 * a partial final code point before passing external text to LVGL. */
size_t app_text_sanitize_utf8(char *text, size_t length);
