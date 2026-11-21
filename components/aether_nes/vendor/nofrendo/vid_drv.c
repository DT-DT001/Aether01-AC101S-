/* Modified in the Aether01 firmware import recorded on 2026-11-05. */
#include <string.h>
#include <noftypes.h>
#include <log.h>
#include <bitmap.h>
#include <vid_drv.h>
#include <gui.h>
#include <osd.h>

/* hardware surface */
static bitmap_t *screen = NULL;// 真实硬件屏幕缓冲

/* primary / backbuffer surfaces */
static bitmap_t *primary_buffer = NULL; // 内核画画用的缓冲(主缓冲)

static viddriver_t *driver = NULL;// 平台交上来的那张函数表

/* fast automagic loop unrolling */
#define  DUFFS_DEVICE(transfer, count) \
{ \
   register int n = (count + 7) / 8; \
   switch (count % 8) \
   { \
   case 0:  do {  { transfer; } \
   case 7:        { transfer; } \
   case 6:        { transfer; } \
   case 5:        { transfer; } \
   case 4:        { transfer; } \
   case 3:        { transfer; } \
   case 2:        { transfer; } \
   case 1:        { transfer; } \
            } while (--n > 0); \
   } \
}

/* some system dependent replacement routines (for speed) */
INLINE int vid_memcmp(const void *p1, const void *p2, int len)
{
   /* check for 32-bit aligned data */
   if (0 == (((uint32) p1 & 3) | ((uint32) p2 & 3)))
   {
      uint32 *dw1 = (uint32 *) p1;
      uint32 *dw2 = (uint32 *) p2;

      len >>= 2;

      DUFFS_DEVICE(if (*dw1++ != *dw2++) return -1, len);
   }
   else
   /* fall back to 8-bit compares */
   {
      uint8 *b1 = (uint8 *) p1;
      uint8 *b2 = (uint8 *) p2;
      
      DUFFS_DEVICE(if (*b1++ != *b2++) return -1, len);
   }
   
   return 0;
}

/* super-dooper assembly memcpy (thanks, SDL!) */
#if defined(__GNUC__) && defined(i386)
#define vid_memcpy(dest, src, len) \
{ \
   int u0, u1, u2; \
   __asm__ __volatile__ ( \
      "  cld            \n" \
      "  rep            \n" \
      "  movsl          \n" \
      "  testb $2,%b4   \n" \
      "  je    1f       \n" \
      "  movsw          \n" \
      "1:               \n" \
      "  testb $1,%b4   \n" \
      "  je 2f          \n" \
      "  movsb          \n" \
      "2:               \n" \
      : "=&c" (u0), "=&D" (u1), "=&S" (u2) \
      : "0" ((len)/4), "q" (len), "1" (dest), "2" (src) \
      : "memory"); \
}
#else /* !(defined(__GNUC__) && defined(i386)) */
INLINE void vid_memcpy(void *dest, const void *src, int len)
{
   uint32 *s = (uint32 *) src;
   uint32 *d = (uint32 *) dest;

   ASSERT(0 == ((len & 3) | ((uint32) src & 3) | ((uint32) dest & 3)));
   len >>= 2;

   DUFFS_DEVICE(*d++ = *s++, len);
}
#endif /* !(defined(__GNUC__) && defined(i386)) */


/* TODO: any way to remove this filth (GUI needs it)? */
bitmap_t *vid_getbuffer(void)
{
   return primary_buffer;
}

void vid_setpalette(rgb_t *p)
{
   ASSERT(driver);
   ASSERT(p);

   driver->set_palette(p);
}

/* blits a bitmap onto primary buffer */
void vid_blit(bitmap_t *bitmap, int src_x, int src_y, int dest_x, int dest_y, 
              int width, int height)
{
   int bitmap_pitch, primary_pitch;
   uint8 *dest_ptr, *src_ptr;

   ASSERT(bitmap);

   /* clip to source */
   if (src_y >= bitmap->height)
      return;
   if (src_y + height > bitmap->height)
      height = bitmap->height - src_y;

   if (src_x >= bitmap->width)
      return;
   if (src_x + width > bitmap->width)
      width = bitmap->width - src_x;

   /* clip to dest */
   if (dest_y + height <= 0)
   {
      return;
   }
   else if (dest_y < 0)
   {
      height += dest_y;
      src_y -= dest_y;
      dest_y = 0;
   }

   if (dest_y >= primary_buffer->height)
      return;
   if (dest_y + height > primary_buffer->height)
      height = primary_buffer->height - dest_y;

   if (dest_x + width <= 0)
   {
      return;
   }
   else if (dest_x < 0)
   {
      width += dest_x;
      src_x -= dest_x;
      dest_x = 0;
   }

   if (dest_x >= primary_buffer->width)
      return;
   if (dest_x + width > primary_buffer->width)
      width = primary_buffer->width - dest_x;   

   src_ptr = bitmap->line[src_y] + src_x;
   dest_ptr = primary_buffer->line[dest_y] + dest_x;

   /* Avoid doing unnecessary indexed lookups */
   bitmap_pitch = bitmap->pitch;
   primary_pitch = primary_buffer->pitch;

   /* do the copy */
   while (height--)
   {
      vid_memcpy(dest_ptr, src_ptr, width);
      src_ptr += bitmap_pitch;
      dest_ptr += primary_pitch;
   }
}

static void vid_blitscreen(int num_dirties, rect_t *dirty_rects)
{
   int src_x, src_y, dest_x, dest_y;
   int blit_width, blit_height;

   screen = driver->lock_write();
   if (NULL == screen)
      return;

   /* center in y direction */
   if (primary_buffer->height <= screen->height)
   {
      src_y = 0;
      blit_height = primary_buffer->height;
      dest_y = (screen->height - blit_height) >> 1;
   }
   else
   {
      src_y = (primary_buffer->height - screen->height) >> 1;
      blit_height = screen->height;
      dest_y = 0;
   }

   /* and in x */
   if (primary_buffer->width <= screen->width)
   {
      src_x = 0;
      blit_width = primary_buffer->width;
      dest_x = (screen->width - blit_width) >> 1;
   }
   else
   {
      src_x = (primary_buffer->width - screen->width) >> 1;
      blit_width = screen->width;
      dest_x = 0;
   }
   
   /* should we just copy the entire screen? */
   if (-1 == num_dirties)
   {
      uint8 *dest, *src;

      src = primary_buffer->line[src_y] + src_x;
      dest = screen->line[dest_y] + dest_x;

      while (blit_height--)
      {
         vid_memcpy(dest, src, primary_buffer->width);
         src += primary_buffer->pitch;
         dest += screen->pitch;
      }
   }
   else
   {
      /* we need to blit just a bunch of dirties */
      int i, j, height;
      rect_t *rects = dirty_rects;

      for (i = 0; i < num_dirties && blit_height; i++)
      {
         height = rects->h;
         if (blit_height < height)
            height = blit_height;

         j = 0;
         DUFFS_DEVICE(  
         {
            vid_memcpy(screen->line[dest_y + rects->y + j] + rects->x + dest_x,
                       primary_buffer->line[src_y + rects->y + j] + rects->x + src_x,
                       rects->w);
            j++;
            blit_height--;
         }, height);

         rects++;
      }
   }

   if (driver->free_write)
      driver->free_write(num_dirties, dirty_rects);
}

/* TODO: this code is sickly */

#define  CHUNK_WIDTH    256
#define  CHUNK_HEIGHT   16
#define  MAX_DIRTIES    ((256 / CHUNK_WIDTH) * (240 / CHUNK_HEIGHT))
#define  DIRTY_CUTOFF   ((3 * MAX_DIRTIES) / 4)

void vid_flush(void)
{
   bitmap_t *temp;
   int num_dirties;
   rect_t dirty_rects[MAX_DIRTIES];

   ASSERT(driver);

   if (true == driver->invalidate)
   {
      driver->invalidate = false;
      num_dirties = -1;
   }
   else
   {
      num_dirties = -1;
   }

   if (driver->custom_blit)
      driver->custom_blit(primary_buffer, num_dirties, dirty_rects);
   else
      vid_blitscreen(num_dirties, dirty_rects);

}

/* emulated machine tells us which resolution it wants */
int vid_setmode(int width, int height)
{
   const int overdraw = 8; /* NES PPU renderer expects left/right guard pixels for fine X scroll */

   if (NULL != primary_buffer)
      bmp_destroy(&primary_buffer);

   primary_buffer = bmp_create(width, height, overdraw);
   if (NULL == primary_buffer)
      return -1;

   bmp_clear(primary_buffer, GUI_BLACK);

   return 0;
}

static int vid_findmode(int width, int height, viddriver_t *osd_driver)
{
   if (osd_driver->init(width, height))
   {
      driver = NULL;
      return -1; /* mode not available! */
   }

   /* we got our driver */
   driver = osd_driver;

   /* re-assert dimensions, clear the surface */
   screen = driver->lock_write();
   if (NULL == screen)
   {
      if (driver->shutdown)
         driver->shutdown();
      driver = NULL;
      return -1;
   }

   /* use custom pageclear, if necessary */
   if (driver->clear)
      driver->clear(GUI_BLACK);
   else
      bmp_clear(screen, GUI_BLACK);

   /* release surface */
   if (driver->free_write)
      driver->free_write(-1, NULL);

   log_printf("video driver: %s at %dx%d\n", driver->name,
              screen->width, screen->height);

   return 0;
}

/* This is the interface to the drivers, used in nofrendo.c */
int vid_init(int width, int height, viddriver_t *osd_driver)
{
   if (vid_findmode(width, height, osd_driver))
   {
      log_printf("video initialization failed for %s at %dx%d\n",
                 osd_driver->name, width, height);
      return -1;
   }
	log_printf("vid_init done\n");

   return 0;
}

void vid_shutdown(void)
{
   if (NULL == driver)
      return;

   if (NULL != primary_buffer)
      bmp_destroy(&primary_buffer);

   if (driver && driver->shutdown)
      driver->shutdown();
   screen = NULL;
   driver = NULL;
}
