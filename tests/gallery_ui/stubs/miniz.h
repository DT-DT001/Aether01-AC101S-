#pragma once

#include <stddef.h>
#include <stdint.h>

/* Fixture-only legacy inflater. New writes run the production RGB565 codec. */
typedef struct { unsigned state; } tinfl_decompressor;
enum { TINFL_FLAG_USING_NON_WRAPPING_OUTPUT_BUF = 4, TINFL_STATUS_DONE = 0 };
#define tinfl_init(decoder) ((decoder)->state = 0)
int tinfl_decompress(tinfl_decompressor *, const uint8_t *, size_t *, uint8_t *, uint8_t *, size_t *, unsigned);
