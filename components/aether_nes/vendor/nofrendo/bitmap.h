
#ifndef _BITMAP_H_
#define _BITMAP_H_

#include <noftypes.h>

/* a bitmap rectangle */
typedef struct rect_s
{
   int16 x, y;
   uint16 w, h;
} rect_t;

typedef struct rgb_s
{
   int r, g, b;
} rgb_t;

typedef struct bitmap_s
{
   int width, height, pitch;
   bool hardware;             /* is data a hardware region? */
   uint8 *data;               /* protected */
   uint8 *line[];  /* will hold line pointers */
} bitmap_t;

extern void bmp_clear(const bitmap_t *bitmap, uint8 color);
extern bitmap_t *bmp_create(int width, int height, int overdraw);
extern bitmap_t *bmp_createhw(uint8 *addr, int width, int height, int pitch);
extern void bmp_destroy(bitmap_t **bitmap);

#endif /* _BITMAP_H_ */
