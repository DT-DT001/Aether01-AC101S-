#pragma once
#include <stdint.h>
/* Let the real IDF ffunicode.c compile on the host without its SDK ffconf. */
#define FF_DEFINED 80386
#define FF_USE_LFN 1
#define FF_CODE_PAGE 936
typedef uint16_t WCHAR;
typedef uint16_t WORD;
typedef uint32_t DWORD;
typedef unsigned int UINT;
WCHAR ff_oem2uni(WCHAR, WORD);
