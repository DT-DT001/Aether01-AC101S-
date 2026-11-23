#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { APP_LYRICS_READ_LIMIT = 24 * 1024 };
typedef struct app_lyrics app_lyrics_t;

/* Parses already-decoded UTF-8. If the file was truncated, omit its last
 * incomplete line. The returned document can be freed with free(). */
app_lyrics_t *app_lyrics_parse(const char *text, bool complete);
size_t app_lyrics_count(const app_lyrics_t *lyrics);
size_t app_lyrics_at(const app_lyrics_t *lyrics, uint32_t position_ms);
const char *app_lyrics_line(const app_lyrics_t *lyrics, size_t index);
